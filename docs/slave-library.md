# Slave Library

Interrupt-driven HUBUS slave for tinyAVR microcontrollers, in `slave/`. The protocol logic lives in the hardware-independent core (`core/hubus_slave_core.c`, shared with the master and the host tests) and follows [§7](spec/07-slave.md); `slave/` adds the public API and one small hardware abstraction layer (HAL) per device family.

## Usage

```c
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include "hubus_slave.h"

static const uint8_t hwid[] PROGMEM = {0x81 /* type */, 0x01 /* variant */, 0x01 /* version */, 0x00 /* revision */,
                                       0x00, 0x00, 0x00, 0x2A /* serial number */};

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
    static hubus_slave_config_t cfg = {
        .hwid = hwid, .hwid_len = sizeof hwid,
        .on_status_capture = on_status_capture, .on_command = on_command,
    };
    cfg.mcid = hubus_slave_mcid_from_eeprom(3); /* written when flashing, else 3 (see Assigning the MCID) */
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
| `hubus_slave_init(cfg)` | Configures the pins, starts the protocol engine, and enables the pin change interrupts. Global interrupts are left to the application: it calls `sei()` after its own setup. Returns 0, and the slave stays silent, for an MCID outside `1..63`, a missing HWID or one longer than 30 bytes, a state longer than 31 bytes, or a speed class above 15. |
| `hubus_slave_write(data, len, more)` | Writes the event buffer: overwrites unread content, increments `GEN`, sets the pending flag, and announces the data ([§4.8](spec/04-frames.md#48-event-buffer), [§6.1](spec/06-operation.md#61-idle-event-handshake)). `more = 1` marks a fragment followed by more ([§4.11](spec/04-frames.md#411-fragments)). Returns `HUBUS_WRITE_OK`, `HUBUS_WRITE_TOO_LONG`, `HUBUS_WRITE_BUSY` (the buffer is being transmitted, [§3.6](spec/03-transactions.md#36-transaction-end-and-buffer-clearing); retry later), or `HUBUS_WRITE_DISABLED` (init failed). Safe from the main loop and from callbacks. |
| `hubus_slave_is_pending()` | 1 while the buffer has not been read. Write the next fragment when it is 0. |
| `hubus_slave_blind_begin()`, `hubus_slave_blind_end()` | Frame a blind section ([§7.8](spec/07-slave.md#78-blind-sections)): code that has to run with all interrupts disabled for longer than the speed class allows. `begin` returns 1 with interrupts disabled, or 0 while the slave drives `DATA` or owes an answer (try again). `end` resynchronizes with the current pin levels and enables interrupts again. Main loop only, with interrupts enabled. |
| `hubus_slave_short_begin()`, `hubus_slave_short_interrupted()`, `hubus_slave_short_end()` | Frame an interruptible section: long timing-critical work in short pieces with all interrupts disabled, which gives way to the bus. `begin` returns 1 with interrupts disabled if the bus is between transactions and no edge waits, else 0 with interrupts enabled (try again). `interrupted` returns 1 once a `CLK` or `DATA` change waits for the interrupt handler; the caller then stops. `end` enables interrupts again. Main loop only. |
| `hubus_slave_mcid_from_eeprom(fallback)` | The MCID in the last EEPROM byte, or `fallback` if that byte holds none (erased, `0`, above `63`): the recommended source of `cfg.mcid` ([assigning the MCID](#assigning-the-mcid)). AVR only. |
| `hubus_slave_mcid_or(mcid, fallback)` | The range check alone: `mcid` if it is `1..63`, else `fallback`, for an MCID from elsewhere. |
| `hubus_slave_isr()` | The pin change handler. The HAL vectors call it; the application only calls it itself in host tests, where there are no interrupts. |

The functions below the library are the hardware-independent core (`core/hubus_slave_core.h`), used by the library, the host tests, and the bus simulation: `hubus_core_init(core, cfg, clk, data)`, `hubus_core_pins(core, clk, data)` (feed the pin levels after every change), `hubus_core_data_low(core)` (the `DATA` drive to apply: 1 pulls low, 0 releases), `hubus_core_write()`, `hubus_core_is_pending()`, and for blind sections `hubus_core_can_blind()` and `hubus_core_resync(core, clk, data)`. Firmware does not call them; `hubus_core_config_t` is the same type as `hubus_slave_config_t`.

Configuration (`hubus_slave_config_t`):

| Field | Meaning |
|---|---|
| `mcid` | `1..63`, unique on the bus ([§7.2](spec/07-slave.md#72-mcid)); where it comes from is up to the application ([assigning the MCID](#assigning-the-mcid)) |
| `hwid`, `hwid_len` | Hardware identifier in `SLAVE_INFO`, in flash (`PROGMEM`), `1..30` bytes: the type alone in the short form, or type, variant, version, hardware revision, and an optional serial number in the full form ([§4.7](spec/04-frames.md#hardware-identifier-hwid), [types](hwid-types.md)) |
| `serial`, `serial_len` | With `HUBUS_ENABLE_SERIAL`: a serial number in RAM, sent after `hwid` and counted in its 30 bytes. The application reads it before `hubus_slave_init()`, for example from EEPROM or the signature row. |
| `state`, `state_len` | Live state for `REQUEST_STATE`: RAM the application keeps up to date, read while transmitting, 1..31 bytes; `NULL` for no state ([§4.10](spec/04-frames.md#410-state)) |
| `on_status_capture` | Writes the current status with `hubus_slave_write()`; `NULL`: `STATUS_CAPTURE` not supported; its module has to say the same |
| `on_command` | Application commands, opcodes `0x10..0xFF`; may be `NULL` |
| `ctx` | Passed to the callbacks |
| `speed_class` | Speed class `0..15` of this firmware, announced in `SLAVE_INFO` byte 0; `0` (default): not declared ([§1.5](spec/01-physical-layer.md#15-speed-classes)) |
| `has_blind_time` | `1`: the firmware may be blind after a command, for the time its module states, and keeps quiet until the next START or RESET; `0` (default): never blind ([§7.8](spec/07-slave.md#78-blind-sections)) |

- The speed class depends on the whole firmware, not only on the device: the longest interrupt handler or interrupt-disabled section of the application adds to the library's own response time. Declare the highest class whose edge response time the firmware meets in every situation.
- A blind section does not count toward the speed class. It is the place for timing-critical code after a command, such as sending a WS2812 frame:

  ```c
  /* in the main loop, after the command callback has set `show` */
  while (!hubus_slave_blind_begin()) {
  }
  ws2812_send(frame, len);   /* interrupts disabled */
  hubus_slave_blind_end();
  ```

  The section must end within the blind time its module states, counted from the STOP of the command, and it must not depend on further bus traffic. Code that is short enough for the speed class needs no blind section. With `has_blind_time` the slave does not announce pending data between a command and the next START or RESET, so that an announcement cannot hold up the blind section ([§7.8](spec/07-slave.md#78-blind-sections)).
- An interruptible section is the alternative for work that repeats on its own, without a command, such as the frames of an animation. The slave is never blind: before each piece the code checks for a waiting edge, and gives up the rest of the work when one arrives. The longest piece adds to the response time and counts toward the speed class; nothing is announced to the master.

  ```c
  /* in the main loop, whenever a new frame is ready */
  if (hubus_slave_short_begin()) {        /* 0 while the bus is busy: try again later */
      for (i = 0; i < len && !hubus_slave_short_interrupted(); i++) {
          ws2812_byte(frame[i]);          /* interrupts disabled, about 12 us */
      }
      hubus_slave_short_end();            /* the interrupt handler takes the edge now */
  }
  ```

  The work has to tolerate being cut off: the LED strip example sends an interrupted frame again later ([LED strip](example-ledstrip.md#animations)).
- `hubus_slave_write()` and `hubus_slave_is_pending()` exist only with the event buffer; `on_status_capture` is ignored without it.
- Callbacks run in the interrupt handler and must be short. Group commands are executed without any response; results go through the event buffer.
- Writes from the main loop disable interrupts for the duration of the copy (at most 31 bytes).

## Optional Parts

The event buffer and the state ([§7.4](spec/07-slave.md#74-optional-parts)) are independent build options. Both are on by default, and every combination is valid. Flash and RAM of the reference firmware on the ATtiny85:

| `HUBUS_ENABLE_EVENTS` | `HUBUS_ENABLE_STATE` | Device | Flash / RAM (bytes) |
|---|---|---|---:|
| 1 | 1 | push button with readable inputs | 1906 / 100 |
| 1 | 0 | event source only | 1842 / 97 |
| 0 | 1 | sensor that is only read, relay with readback | 1420 / 59 |
| 0 | 0 | relay without feedback (the master caches its state) | 1360 / 56 |

The event buffer costs about 490 bytes of flash and 41 bytes of RAM (including the capture and write code of the reference firmware), the state about 60 bytes of flash and 3 bytes of RAM.

## Examples

`examples/` has complete firmware for the ATtiny85, one for each combination of the optional parts. The CMake build compiles each example with its own options, independent of the options of the build directory.

| Example | Device | Events | State |
|---|---|---|---|
| `slave-switch` | push button: announces changes, input level readable | 1 | 1 |
| `slave-relay` | relay: state readable, `STATUS_CAPTURE` | 1 | 1 |
| `slave-sensor` | temperature sensor: value collected with `STATUS_CAPTURE` | 1 | 0 |
| `slave-sensor-passive` | temperature sensor that is only read (the master polls) | 0 | 1 |
| `slave-relay-simple` | relay without feedback (the master caches its state) | 0 | 0 |
| `slave-ledstrip` | WS2812B strip driven with raw frames or animated by bytecode from the master, plus a push button ([description](example-ledstrip.md)) | 1 | 1 |
| `slave-dht11` | DHT11 temperature and humidity, read only: state with the values and a fault byte, with firmware update ([description](example-dht11.md)) | 0 | 1 |
| `slave-distance` | HC-SR04 distance: state and event with the distance and a near flag, trigger distance from the master, with firmware update ([description](example-distance.md)) | 1 | 1 |
| `slave-motion` | HC-SR501 PIR motion sensor: an event for every change, with firmware update ([description](example-motion.md)) | 1 | 0 |

Their hardware identifiers are in the [table of types](hwid-types.md) and select the modules built into [hubusd](daemon.md#entities); their modules also state the entities of each device ([hubusd](daemon.md#entities)). The build also produces `example_switch_update`, the switch with [firmware update](#firmware-update), and `example_relay_simple_pins`, the simple relay with its MCID from two jumpers ([assigning the MCID](#assigning-the-mcid)).

Every example takes its MCID from the last EEPROM byte and falls back to its own default (switch 5, relay 3, sensor 7, passive sensor 8, simple relay 4, LED strip 11): one image serves every device of a kind.

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
| `HUBUS_RX_BUF_SIZE` | Largest accepted command payload including the opcode, `1..31`, default 31 (20 on 128 B RAM devices with the event buffer); its module has to state the same. Longer commands are received to the end and ignored ([§4.4](spec/04-frames.md#44-command-opcodes)). |
| `HUBUS_ENABLE_SERIAL` | `1`: `serial` and `serial_len` in the configuration; default `0` |
| `HUBUS_CLK_PCGROUP`, `HUBUS_DATA_PCGROUP` | Pin change group of a pin on classic devices the HAL does not know (see below) |

`DATA` needs an external pull-up to 3.3 V ([§1.1](spec/01-physical-layer.md#11-bus-lines)); the library never enables internal pull-ups.

## Firmware Update

A slave can receive new firmware over the bus if it carries the updater ([§6.8](spec/06-operation.md#68-firmware-update)). This is an optional part: the updater takes 2 KB of flash, so it suits devices with 8 KB or more. There are two:

| Updater | Devices | Layout |
|---|---|---|
| `updater/hubus_updater_avr.c` | ATtiny85 (classic tinyAVR) | updater at the end of flash, application from 0, reset vector rule |
| `updater/hubus_updater_modern.c` | tinyAVR 0/1/2-series with 8 KB or more: ATtiny804/814/824, 1604/1614/1616/1624 and the other 16 KB parts, 3216/3217/3224 | updater in the boot section at 0, application behind it ([below](#tinyavr-012-series)) |

Both share the protocol part (`updater/hubus_updater.c`) and are built and size-checked for every device of the [matrix](#supported-devices). The ATtiny85 updater also runs on a simulated device; the 0/1/2-series cannot be simulated, so its updater is tested as protocol logic on the simulated bus with its layout (`test_update_boot_section`). Neither has run on hardware.

| ATtiny85 flash | Content |
|---|---|
| `0x0000..0x17BF` | application, at most 6080 bytes; word 0 jumps to the updater |
| `0x17C0..0x17FF` | info page: length, CRC-16, and entry of the application |
| `0x1800..0x1FFF` | updater (`updater/`, 2040 bytes; 1980 without `UPDATE_SET_MCID`) |

**Application.** Build it with `HUBUS_ENABLE_UPDATE=1` and `HUBUS_UPDATER_ADDR=0x1800`, and keep it below `0x17C0`; the firmware only needs a HWID whose module states the update support. The library then jumps to the updater on `UPDATE_ENTER`, with the MCID handed over in `GPIOR1`. This costs about 60 bytes of flash. Without an updater at that address the command is ignored. `example_switch_update` is the switch built this way, with a check that it fits.

**Updater.** `build/avr-attiny85/updater/hubus_updater.hex`, linked to `0x1800`. It runs without interrupts, starts the application at reset if the info page checks out (a CRC-16 over the application, about 30 ms at 8 MHz), and otherwise waits for the master. Its MCID is the last EEPROM byte, the same one the library reads ([assigning the MCID](#assigning-the-mcid)); while that byte holds none, the build option `HUBUS_UPDATER_MCID` (default 63) applies. At `UPDATE_ENTER` it stores the MCID the application hands over there. The application may read the byte, but must not write it.

| Updater option (CMake) | Meaning |
|---|---|
| `HUBUS_UPDATER_MCID` | MCID while the last EEPROM byte holds none; default 63 |
| `HUBUS_UPDATER_SET_MCID` | `ON` (default): the updater takes `UPDATE_SET_MCID`, the change of the MCID over the bus, and announces it with flag bit 1 ([§6.8](spec/06-operation.md#mcid)). It costs 60 bytes, which the 2 KB of the updater have to spare; the build also produces `hubus_updater_no_set_mcid` to show the difference. `OFF` leaves it out (C macro `HUBUS_UPD_SET_MCID`). |

**Installing.** The first installation needs an in-system programmer, once: the [fuses](#fuses), the updater, and its MCID. `tools/flash-avr.sh` writes the updater image and the MCID in one call:

```sh
avrdude -c usbasp -p t85 -U lfuse:w:0xE2:m -U hfuse:w:0xD5:m -U efuse:w:0xFE:m
sh tools/flash-avr.sh build/avr-attiny85/updater/hubus_updater.hex 5
```

The updater image also holds the first word of flash, the jump to the updater, so the freshly programmed device starts the updater. It waits for the master under MCID 5, and `hubus_master_example update 5 example_switch_update.hex` loads the switch ([master library](master-library.md#firmware-update)), which starts under the same MCID, since it reads the same byte. A device programmed without an MCID waits under 63; `hubus_master_example mcid 63 5` gives it its MCID over the bus first ([below](#changing-the-mcid-over-the-bus)). Writing the application with the programmer as well does not work: its first word would not jump to the updater, and the info page would be missing.

A firmware update over the bus writes the flash only: the MCID in the EEPROM survives it.

### tinyAVR 0/1/2-Series

These devices have a boot section in hardware, at the start of flash, and always start there. The updater is that boot section; the application follows it with its own vector table, which the CPU uses there by itself. Only code in the boot section can write the application area, and nothing can write the boot section, so an application can never damage its updater. No reset vector rule is needed (flag bit 0 is `0`), and the updater announces the area behind it.

| ATtiny1614 flash | Content |
|---|---|
| `0x0000..0x07FF` | boot section: updater (2010 bytes; 1938 without `UPDATE_SET_MCID`) |
| `0x0800..0x3FBF` | application, at most 14272 bytes, linked to `0x0800` |
| `0x3FC0..0x3FFF` | info page |

The 32 KB devices have 128-byte pages, so their info page is `0x7F80..0x7FFF`. The updater build prints the fuse values it needs.

| CMake option | Meaning |
|---|---|
| `HUBUS_UPDATER_BOOT_SIZE` | Size of the boot section in bytes, a multiple of 256 (default 2048): the updater must fit, the application starts behind it. Fuse `BOOTEND` = this value / 256. |

**Application.** Build it with `HUBUS_ENABLE_UPDATE=1` and link it to the end of the boot section, `-Wl,--section-start=.text=0x800` (`ld` reads the address as hexadecimal). On `UPDATE_ENTER` the library jumps to address 0 with the MCID in `GPIOR1`. `hubus_size_reference_update` in the AVR build is the reference firmware built this way, with a check that it fits. An image from `avr-objcopy -O ihex` starts at `0x0800`; `hubus_master_example update` reads the start from the file and refuses an image for another area ([master library](master-library.md#firmware-update)). A module image (`avr-objcopy -O binary`) starts at the start of the area by itself.

**Updater.** `build/avr-attiny1614/updater/hubus_updater.hex`, linked to 0. It differs from the one for classic devices in a few points:

- **No C runtime.** It has no vector table, since it uses no interrupts. That saves 124 bytes of the 2 KB on devices with 16 KB and more. Its start code sets the stack pointer itself, because the application also jumps there.
- **Flash and EEPROM writes.** It writes the flash through `NVMCTRL`, with the page buffer filled through the mapped flash, and the MCID through the mapped EEPROM. Both share the page buffer, so a page write waits for a pending EEPROM write.
- **Clock.** It starts a valid application at the reset clock (20 or 16 MHz divided by 6), which the application expects. If it stays, it runs from the internal oscillator divided by 2 (10 or 8 MHz) to poll the bus.
- **When the application hands over.** It switches the watchdog off and returns the ports, including `PINnCTRL` with pull-ups and pin interrupts, to their reset state.
- **Reset.** After `UPDATE_LEAVE` it uses the software reset.

**Installing.** Over UPDI, once: the fuses, the updater, and its MCID.

```sh
avrdude -c serialupdi -P /dev/ttyUSB0 -p t1614 -U bootend:w:0x08:m -U append:w:0x00:m -U syscfg0:w:0xF7:m -U bodcfg:w:0x45:m
sh tools/flash-avr.sh -m attiny1614 -c serialupdi -P /dev/ttyUSB0 build/avr-attiny1614/updater/hubus_updater.hex 5
```

| Fuse | Value | Why |
|---|---|---|
| `bootend` (fuse 8) | `0x08` | boot section of 8 × 256 bytes: the updater (`HUBUS_UPDATER_BOOT_SIZE` / 256) |
| `append` (fuse 7) | `0x00` | the application code section reaches to the end of flash, so the updater may write all of it |
| `syscfg0` (fuse 5) | `0xF7` | the default `0xF6` with `EESAVE`: a chip erase keeps the EEPROM and with it the MCID; the `UPDI` pin stays the programming pin |
| `bodcfg` (fuse 1) | `0x45` | brown-out detection at 2.6 V, active and in sleep, against a sinking supply during flash and EEPROM writes ([§8.10](spec/08-faults.md#810-power-loss-during-a-firmware-update)) |

`wdtcfg` (fuse 0) must not lock the watchdog on, because the updater switches it off. As on the ATtiny85, the device then waits in its updater under its MCID, and the master loads the application over the bus. An application written with the programmer would not start: its info page is missing.

## Assigning the MCID

The library takes `cfg.mcid` and nothing else: where the MCID comes from is the choice of the application. Everything below is a recommendation and a set of building blocks that the developer of a firmware selects, as with the optional parts; none of it is tied to a controller.

| Source | How | Example |
|---|---|---|
| **EEPROM, written when flashing** (recommended) | one image for all devices, one byte per device: `cfg.mcid = hubus_slave_mcid_from_eeprom(default)` | every example |
| Pins (jumpers, DIP switch) | read once at reset, a base MCID plus the pins | `example_relay_simple_pins`: `0x20` plus jumpers on PB1 and PB4 |
| A constant | one image per device | – |

**Recommended: the last EEPROM byte.** `hubus_slave_mcid_from_eeprom(default)` reads the last byte of the EEPROM (`E2END` on classic tinyAVR, the last byte of the mapped EEPROM on the 0/1/2-series) and returns `default` if it holds no MCID. The updater keeps its MCID in the same byte, so application and updater always agree. `tools/flash-avr.sh` writes it together with the image:

```sh
sh tools/flash-avr.sh build/avr-attiny85/examples/example_relay.hex 12   # image and MCID 12, one avrdude call
sh tools/flash-avr.sh - 13                                               # the MCID alone, the flash stays
sh tools/flash-avr.sh -m attiny1614 -c serialupdi -P /dev/ttyUSB0 image.hex 7
```

The script builds a one-byte Intel HEX image for the EEPROM (`-n` prints it and the avrdude command). It needs avrdude 7.0 or newer, which writes only that byte, and `avr-gcc` for the size of the EEPROM. Without an MCID it says so: the firmware then runs with its default.

**Without a value per device** every device of a firmware gets its default, and identical devices collide. That is accepted: the commissioning scan reports them as a suspected duplicate MCID ([§6.4](spec/06-operation.md#64-commissioning-scan-and-duplicate-mcids)), and one of them has to be given its MCID with the programmer or over the bus. There is no automatic enumeration: without a unique serial number the master cannot address one of two devices that answer to the same MCID.

**Pins** make the MCID visible on the device and need no programmer, at the cost of pins. The updater stores whatever the application hands over at `UPDATE_ENTER`, so it agrees with the pins after the first update; a change over the bus does not last, since the application reads its pins again (the master reports it and undoes the change).

### Changing the MCID over the Bus

With an updater that takes `UPDATE_SET_MCID` (`HUBUS_UPDATER_SET_MCID`), the master can give a slave another MCID without a programmer ([§6.8](spec/06-operation.md#mcid)): `hubus_master_set_mcid()` ([master library](master-library.md#firmware-update)), `hubus_master_example mcid <old> <new>`, or the `mcid` topic of [hubusd](daemon.md#changing-the-mcid). The slave switches to its updater, which writes the new MCID to the last EEPROM byte and answers to it at once; the application then starts with it if it reads that byte. It works for a slave in its updater as well, for example a fresh device under the default MCID 63, before its first application.

This is only possible in update mode, so only for firmware with `HUBUS_ENABLE_UPDATE` and an updater with the option. A firmware without update support keeps the MCID it was flashed with.

### Fuses

The fuses of a slave with the updater, in one place (ATtiny85, internal 8 MHz oscillator; the tinyAVR 0/1/2-series [above](#tinyavr-012-series)):

| Fuse | Value | Why |
|---|---|---|
| `lfuse` | `0xE2` | internal 8 MHz oscillator, no clock division |
| `hfuse` | `0xD5` | `EESAVE` programmed: a chip erase keeps the EEPROM and with it the MCID; brown-out detection at 2.7 V protects flash and EEPROM writes against a sinking supply ([§8.10](spec/08-faults.md#810-power-loss-during-a-firmware-update)); `WDTON` unprogrammed, since the updater switches the watchdog off; `SPIEN` programmed for the programmer |
| `efuse` | `0xFE` | `SELFPRGEN` programmed: the updater may write the flash |

Every `avrdude -U flash:w` erases the chip first. Without `EESAVE` (`hfuse` `0xDD`) that erases the EEPROM too, and the MCID with it, unless the same call writes it again, as `tools/flash-avr.sh` does. A slave without the updater needs only `lfuse`; `EESAVE` is still useful there to keep the MCID across reprogramming.

## Supported Devices

The HAL is selected from the compiler's device macro (`-mmcu=...`):

| Family | Devices | HAL |
|---|---|---|
| Classic tinyAVR | ATtiny25/45/85, 24/44/84, 261/461/861, 48/88, 441/841, 2313/2313A/4313, 1634 | `hal/hubus_hal_classic.h`: `DDRx`/`PORTx`/`PINx`, pin change interrupts |
| tinyAVR 0/1/2-series | ATtiny202/402/412, 804/814/1614/1616, 3216/3217, 424/824/1624 | `hal/hubus_hal_modern.h`: `PORTx`/`VPORTx`, per-pin interrupt sense, `PORTx_PORT_vect` |
| Host (tests) | – | `hal/hubus_hal_host.h`: pins are variables |

- **Reference target:** the ATtiny85 is always fully supported, with 31-byte payloads in both directions. Every library change must keep its reference build within the budget of 2050 bytes of flash and 112 bytes of RAM; the CMake build fails otherwise (`HUBUS_BUDGET_FLASH`, `HUBUS_BUDGET_RAM` in `slave/CMakeLists.txt`).
- **Minimum device:** 2 KB flash and 128 B RAM. Smaller devices (for example ATtiny13A) and the reduced-core ATtiny4/5/9/10 are not supported. On 2 KB devices the library with both optional parts leaves about 120 bytes of flash for the application, without the event buffer about 600.
- **128 B RAM devices** (ATtiny24/25, 261, 2313/2313A, 202/204/212/214) default to 20-byte buffers when they have the event buffer: with two 31-byte buffers the library takes 94 bytes of RAM, which leaves too little for the stack (the interrupt path alone needs about 32 bytes with short callbacks, measured in `simavr`, [development](development.md#avr-tests)). Without the event buffer only the command buffer remains, and 31 bytes fit.
- **Pins:** `CLK` and `DATA` can be any pins with pin change or edge interrupts. For every classic device listed above the HAL derives the pin change group (register, enable bit, vector) from the port; for other classic devices `HUBUS_CLK_PCGROUP` and `HUBUS_DATA_PCGROUP` name it. On classic devices, both pins on the same port share one interrupt vector.
- Build-tested with `gcc-avr` 14.x for every device listed (`tools/build-avr-matrix.sh`). The ATtiny85 build runs in `simavr` against the real master ([AVR tests](development.md#avr-tests)); the 0/1/2-series cannot be simulated. None of the HALs has run on real hardware yet ([open issues](project/open-issues.md)).

## Behavior Notes

- The slave has no timers and no timeouts ([§7.6](spec/07-slave.md#76-no-timeouts)).
- A buffer write is refused while the `SLAVE_DATA` response is being transmitted; a write between the end of the response and the STOP is kept ([§3.6](spec/03-transactions.md#36-transaction-end-and-buffer-clearing)).
- Event buffer, `GEN`, and pending flag survive a RESET ([§6.5](spec/06-operation.md#65-communication-stack-reset)).
