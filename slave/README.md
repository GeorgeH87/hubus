# HUBUS Slave Library

Interrupt-driven HUBUS slave for tinyAVR microcontrollers. The protocol logic lives in the hardware-independent core (`../core/`, shared with the master and the host tests); this directory contains the public API and one small HAL per device family.

## Usage

```c
#include <avr/pgmspace.h>
#include "hubus_slave.h"

static const uint8_t info[] PROGMEM = {0x01 /* device type */, 0x01 /* firmware */, 0x00, 0x00, 0x00, 0x2A /* serial number */};

static void on_status_capture(void *ctx)
{
    uint8_t state = read_state();
    hubus_slave_write(&state, 1, 0);          /* the status becomes the buffer content */
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
    hubus_slave_init(&cfg);                    /* enables the pin change interrupts and sei() */
    for (;;) {
        /* application; hubus_slave_write() announces new data to the master */
    }
}
```

See `../examples/` for complete relay, switch, and temperature sensor firmware for the ATtiny85.

## API

| Function | Purpose |
|---|---|
| `hubus_slave_init(cfg)` | Configure the pins, start the protocol engine, enable interrupts. Returns 0 (and the slave stays silent) for MCID outside `1..63` or more than 28 info bytes. |
| `hubus_slave_write(data, len, more)` | Write the single outgoing buffer: overwrites unread content, increments `GEN`, sets the pending flag, announces the data (idle event handshake). `more = 1` marks a fragment that is followed by further fragments. Returns `HUBUS_WRITE_OK`, `HUBUS_WRITE_TOO_LONG`, or `HUBUS_WRITE_BUSY` (the buffer is being transmitted; retry later). Safe from the main loop and from callbacks. |
| `hubus_slave_is_pending()` | 1 while the buffer has not been read. Write the next fragment when it is 0. |

Callbacks (`on_status_capture`, `on_command`) run in the interrupt handler and must be short. `on_status_capture == NULL` clears the capability bit in `SLAVE_INFO`. Group and broadcast commands are executed without any response; results go through the buffer.

## Configuration

Set on the compiler command line (the CMake build takes `-DHUBUS_CLK_PORT=...` etc.):

| Macro | Meaning |
|---|---|
| `HUBUS_CLK_PORT`, `HUBUS_CLK_PIN` | Port letter and bit of `CLK` (default on ATtiny25/45/85: `B`, `2`) |
| `HUBUS_DATA_PORT`, `HUBUS_DATA_PIN` | Port letter and bit of `DATA` (default on ATtiny25/45/85: `B`, `3`) |
| `HUBUS_TX_BUF_SIZE` | Outgoing buffer size, `1..31`, default 31 |
| `HUBUS_RX_BUF_SIZE` | Largest accepted command payload including the opcode, `1..31`, default 31; announced in `SLAVE_INFO` byte 2. Longer commands are received to the end and ignored. |

`DATA` needs an external pull-up to 3.3 V; the library never enables internal pull-ups. Both pins need pin change / edge interrupts.

## Supported Devices

The HAL is selected from the compiler's device macro (`-mmcu=...`).

| Family | HAL | Pin change group |
|---|---|---|
| Classic tinyAVR: ATtiny25/45/85, 24/44/84, 441/841, 48/88, 261/461/861 | `hal/hubus_hal_classic.h` | derived from the port; other classic devices: define `HUBUS_CLK_PCGROUP` / `HUBUS_DATA_PCGROUP` |
| tinyAVR 0/1/2-series: ATtiny202/402/412, 804/814/1614/1616, 3216/3217, 424/824/1624 | `hal/hubus_hal_modern.h` | per-pin interrupt sense, `PORTx_PORT_vect` |
| Host (tests) | `hal/hubus_hal_host.h` | pins are variables |

Build-tested with `gcc-avr` 14.x for ATtiny25, 45, 85, 24, 44, 84, 441, 841, 48, 88, 261, 461, 861, 202, 402, 814, 1614, 1624, 3216, 424, and 824. Only the classic devices can be simulated with `simavr`; none of the HALs has been run on real hardware yet (`../TODO.md` section 9).

## Footprint

Reference firmware (`size_reference.c`: complete API, status capture, command callback, `SLAVE_INFO` from flash), ATtiny85, 31-byte buffers, `-Os`:

| | bytes |
|---|---:|
| Flash | about 2050 (about 1950 without `REQUEST_STATE`) |
| RAM (`.bss`) | 106 (103 without `REQUEST_STATE`; of which both 31-byte buffers: 62) |

The CMake build fails if the ATtiny85 reference build exceeds its budget (`HUBUS_BUDGET_FLASH`, `HUBUS_BUDGET_RAM` in `CMakeLists.txt`): 2050 / 112.

`REQUEST_STATE` support (`HUBUS_ENABLE_STATE`, see `hubus_slave_config.h`) costs about 100 bytes of flash and 3 bytes of RAM. It is on by default and off for the 2 KB devices (ATtiny24/25), where the library alone takes most of the flash and only about 100 bytes remain for the application. A slave built without it simply never answers `REQUEST_STATE`.

## Behavior Notes

- A slave has no timers and no timeouts; every state change is caused by a `CLK` edge or a START/STOP/RESET condition.
- A buffer write is refused (`HUBUS_WRITE_BUSY`) while the `SLAVE_DATA` response is being transmitted, so a frame never mixes old and new content. A write between the end of the response and the STOP keeps the new content (the STOP does not clear it).
- The outgoing buffer, the `GEN` counter, and the pending flag survive a RESET.
- Writes from the main loop disable interrupts for the duration of the copy (at most 31 bytes).
