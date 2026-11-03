# Hardware Identifier Types

The central table of the types in the hardware identifier ([§4.7](spec/04-frames.md#hardware-identifier-hwid)). A new device gets its type or variant here. Each row has a module in `daemon/hubusd_modules.c` or `daemon/hubusd_ledstrip.c` ([§6.10](spec/06-operation.md#610-master-modules)), which also lists the [entities](daemon.md#entities) of the device.

| Type | Variant | Device | Firmware | Module |
|---|---|---|---|---|
| `0x81` | `1` | Relay with readback | `examples/slave-relay` | `relay` |
| `0x81` | `2` | Relay without feedback | `examples/slave-relay-simple` | `relay-simple` |
| `0x82` | `1` | Switch or push button | `examples/slave-switch` | `switch` |
| `0x82` | `2` | The same with firmware update | `example_switch_update` | `switch-update` |
| `0x83` | `1` | Temperature sensor that reports changes | `examples/slave-sensor` | `temperature` |
| `0x83` | `2` | Temperature sensor that is only read | `examples/slave-sensor-passive` | `temperature-passive` |
| `0x84` | `1` | LED strip and push button | `examples/slave-ledstrip` | `ledstrip` |
| `0x85` | `1` | Dimmer: only the [example of a plugin](master-library.md#loading-plugins), no firmware in this repository | – | `dimmer` |
| `0xFF` | – | Updater, on every device | `updater/` | `updater` |

| Range | Use |
|---|---|
| `0x01..0x7F` | Short form; none assigned yet. |
| `0x80`, `0x86..0xEF` | Full form, free. |
| `0xF0..0xFE` | Reserved for tests. |
