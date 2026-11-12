# Development

## Environment

Development runs in WSL2 with Ubuntu 26.04 LTS on Windows. The same Ubuntu setup builds the host tests, the bus simulation, the master, the daemon, the AVR slave firmware, and the documentation.

Create a dedicated distribution (PowerShell):

```powershell
wsl --install Ubuntu-26.04 --name hubus
```

Inside the distribution, from the repository root:

```sh
sudo sh tools/setup-ubuntu.sh     # toolchain and libraries
sh tools/setup-docs.sh            # documentation tools (only for working on the documentation)
```

The repository can stay on the Windows file system and is reachable from WSL at `/mnt/c/...`; builds are small, so the slower cross-file-system access is acceptable. Recommended Git settings inside WSL:

```sh
git config --global core.autocrlf input
git config --global --add safe.directory /mnt/c/Users/<user>/Projekte/hubus
```

Editor: VS Code with the extensions `ms-vscode-remote.remote-wsl`, `ms-vscode.cpptools`, and `ms-vscode.cmake-tools`, opened from inside WSL (`code .`). The CMake presets `host` and `avr` (`CMakePresets.json`) are picked up by CMake Tools.

## Toolchain

Verified versions (2026-10-21):

| Tool | Version | Purpose |
|---|---|---|
| `gcc` | 15.2 | host build (tests, simulation, master, daemon) |
| `cmake` | 4.2 | build system |
| `ninja` | 1.13 | build backend |
| `gcc-avr` | 14.2 | slave firmware |
| `avr-libc` | 2.2.1 | slave firmware |
| `avrdude` | 7.1 | flashing (ISP, UPDI via SerialUPDI) |
| `simavr`, `libsimavr-dev` | 1.6 | slave firmware on simulated devices (`test/avr/`, classic tinyAVR only) |
| `libgpiod-dev` | 2.2.1 | master GPIO access (v2 API) |
| `libyaml-dev`, `libmosquitto-dev`, `mosquitto` | | daemon, and its end-to-end test |
| PlantUML | 1.2026.9 | timing diagrams (`tools/setup-docs.sh`) |
| mermaid-cli | 12.0.0 | checks the Mermaid diagrams (`tools/setup-docs.sh`) |

- `gcc-avr` 14.2 with `avr-libc` 2.2.1 supports classic tinyAVR and tinyAVR 0/1/2-series without Microchip device packs.
- `simavr` only simulates classic devices (for example ATtiny84/85); the 0/1/2-series HAL can be compiled but not simulated. The host build uses its library (`libsimavr-dev`) for the AVR tests ([below](#avr-tests)) and skips them if it is missing.
- The development host ships `libgpiod` v2. Whether the master also needs the v1 API depends on the Raspberry Pi OS version of the target ([open issues, H2](project/open-issues.md#h2-master-clock-implementation)).

## Build and Test

All commands run inside WSL from the repository root (from Windows: `wsl -d hubus --cd /mnt/c/Users/<user>/Projekte/hubus -- sh tools/build-host.sh`).

| Task | Command |
|---|---|
| Host build, unit tests, bus simulation (sanitizers, `-Werror`) | `sh tools/build-host.sh` |
| Slave firmware for the ATtiny85, with the flash/RAM budget check | `sh tools/build-avr.sh` |
| Slave firmware for another device | `sh tools/build-avr.sh attiny841` |
| Size reference firmware for the whole device matrix | `sh tools/build-avr-matrix.sh [mcu ...]` |
| Flash an image and the MCID of the device in one avrdude call | `sh tools/flash-avr.sh [-m mcu] [-c programmer] image.hex [mcid]` ([slave library](slave-library.md#assigning-the-mcid)) |
| Largest symbols of the reference firmware | `sh tools/sizes.sh [mcu]` |
| Sanity check of the test suite (deliberate bugs must be caught) | `sh tools/mutation-check.sh` |
| Only some mutants of the sanity check | `sh tools/run-mutants.sh "<name pattern>" ...` |
| Only the AVR tests (firmware on simulated devices) | `ctest --preset host -L avr -V` |
| One test with the bus trace on stderr | `HUBUS_SIM_TRACE=1 ./build/host/test/test_master` |
| The same, rebuilt, showing only the context of the first matches | `sh tools/run-trace.sh test_master [pattern] [lines]` |
| Render the timing diagrams and check the documentation | `sh tools/build-docs.sh` |

### Repository Layout

| Directory | Content |
|---|---|
| `core/` | Hardware-independent protocol code: CRC, frame codec, arbitration helpers, `GEN` evaluation, slave state machine |
| `slave/` | Slave library: public API and the HALs for classic tinyAVR, tinyAVR 0/1/2-series, and the host |
| `master/` | Master logic on a HAL, and the libgpiod HAL for the Raspberry Pi |
| `daemon/` | `hubusd` |
| `test/` | Unity tests, bus simulation (`test/sim/`), slave firmware on simulated AVR devices (`test/avr/`), test support (`test/support/`), daemon tools (`test/tools/`) |
| `examples/` | Slave firmware examples (ATtiny85) and the master command line example |
| `docs/` | This documentation; `docs/diagrams/` holds the PlantUML sources and their SVGs |
| `cmake/` | AVR toolchain file, device defaults, size check |
| `tools/` | Setup, build, and check scripts |

### Tests

- `test_crc`, `test_frame`, `test_arb`, `test_gen`: the core modules (check value `0xDF`, header layout, exhaustive prefix matching, Hamming distance 4 for 12-byte payloads, arbitration against the example of [§5.6](spec/05-arbitration.md#56-example-four-pending-slaves) and random bursts, `GEN` evaluation).
- `test_slave_core`: the slave state machine with exact wire traffic from a bit-level master (`test/support/rawmaster.h`).
- `test_master`: the real master with real slave cores on the simulated wired-AND bus (startup, bursts, commands, commissioning, fragments, states).
- `test_faults`: fault injection (bit errors, lost and extra clock edges, lost STOP and START, stuck line, noise in the arbitration, an event racing a START, duplicate MCID), against [§8](spec/08-faults.md).
- `test_module`: the master modules ([§6.10](spec/06-operation.md#610-master-modules)): the forms of the hardware identifier, the validation of a module, the selection with the most specific rule and with conflicts, blind times per opcode, and a module function with typed values.
- `test_module_loader`: module plugins ([master library](master-library.md#loading-plugins)), built from `test/plugins/`: loading and selecting, every refusal (interface version, missing or empty entry, name in use, conflicting rules), the order of a directory, a set that is full, files and directories that others can write, in `/tmp`, a plugin file that is replaced, and the example plugin.
- `test_blind`: blind sections after a command ([§7.8](spec/07-slave.md#78-blind-sections)): a slave that ends its section at every possible point of foreign traffic (bursts with arbitration and handoff slots, state reads, commands) stays passive until the next START and disturbs nobody; the stale-level case the resynchronization prevents. A pending slave with a blind time keeps quiet after a command until the next START or RESET.
- `test_update`, `test_update_no_set_mcid`: the firmware update ([§6.8](spec/06-operation.md#68-firmware-update)) with the master library against the updater logic and an application core on the simulated bus, built with and without `UPDATE_SET_MCID`: updates, bit errors, interruptions, refused pages and images, and the change of the MCID (from an application and from the updater, a taken MCID, the checks of the updater, bit errors, power loss in update mode, an application with an MCID of its own, an updater without the option).
- `test_slave_lib`: the public slave API on the host HAL, including the interruptible section.
- `test_ledvm`: the animation bytecode of the LED strip example: the program check, every instruction with its 16-bit semantics, outputs and layouts, the documented programs, and arbitrary code under the sanitizers.
- `test_small_buffers`: the core with small compile-time buffers and without the state.
- `test_slave_passive`, `test_slave_command_only`: the core without the event buffer, with and without the state ([§7.4](spec/07-slave.md#74-optional-parts)).

In every master and fault test the simulation checks two invariants: a slave never changes `DATA` while `CLK` is high, and the master never undercuts its configured timing minimums.

### AVR Tests

The host tests run the slave core compiled for the host. `test_avr_slave` and the other tests with the label `avr` run the slave firmware as it runs on the device: the host build builds a test firmware (`test/avr/firmware/fw_slave.c`, and the examples and the updater as shipped) with `avr-gcc` for the ATtiny85 at 8 MHz, and `simavr` executes it cycle by cycle. Each simulated device is a slave of the bus simulation (`test/avr/avr_slave.c`): the line levels go to its `CLK` and `DATA` pins, its `DDR` register drives `DATA`, and the master's delays are the time base. So the code generated by `avr-gcc`, the pin change interrupt, the classic HAL, `SLAVE_INFO` from flash, the atomic sections, and a blind section with interrupts really disabled run against the real master; the invariants above apply here too, and a device that answers too late breaks them.

- Protocol: `SLAVE_INFO`, commands and state, events from the main loop, `STATUS_CAPTURE`, arbitration of two devices with a host core, a blind section with foreign traffic, and the builds without the event buffer and without both optional parts.
- Measurements, printed with `-V` and checked against limits: the longest interrupt handler, the longest edge response (pin change to the end of the handler that processed it, the `R` of [§1.5](spec/01-physical-layer.md#15-speed-classes)), the deepest stack, and the shortest clock half period at which the bus still works. The test firmware declares speed class 9 (64 µs); with every master minimum set to 64 µs the bus must work.
- Two mutants of `tools/mutation-check.sh` break the classic HAL; only this test catches them.
- Measured with the test firmware (ATtiny85, 8 MHz): longest response 423 cycles (53 µs), at the falling edge after the turnaround slot of an exact request, where the answer starts; stack 34 bytes, of which 32 in the interrupt handler, callbacks included. The bus works down to a half period of 50 µs and fails at 45 µs.
- `test_avr_ledstrip`: the LED strip example as shipped, with its WS2812B output on PB0 decoded bit by bit: the power-up frame, `SHOW`, programs with parameters and time, refused programs, and an animation on a busy bus, where frames are given up but no transaction is lost. Checks the WS2812B bit timing, the low phases inside a frame, and the response time.
- `test_avr_examples`: the other slave examples as shipped. The relays sleep in their main loop and wake up by the pin change interrupt; the switch debounces a button on PB1 (a short press is not announced, an event that comes while the bus is busy still arrives); the sensors measure the temperature channel of the ADC. Four of them share one bus: `STATUS_CAPTURE` and the state reads in descending MCID order. The MCID: two relays from one image under the MCIDs of their EEPROM bytes, the default for a byte without an MCID, and `example_relay_simple_pins` with its MCID from two jumpers ([slave library](slave-library.md#assigning-the-mcid)).
- `test_avr_update`: the updater as built for the ATtiny85 (`updater/hubus_updater_avr.c`), loaded from its HEX file like a device from the programmer, against `hubus_master_update()` with `example_switch_update` as the image: the first install, the replacement of the running application (`UPDATE_ENTER` in its interrupt handler, the MCID in EEPROM), and an update that breaks off and is repeated after a power cycle. Covers self-programming (`SPM`), the CRC check of the info page at boot, and `UPDATE_LEAVE` through the watchdog. The MCID: written by the programmer into the EEPROM, and changed with `hubus_master_set_mcid()` on a fresh device in its updater and on the running switch, which keeps it after a power cycle and an update.
- What the harness adds to `simavr` 1.6: self-programming on the ATtiny25/45/85, which it lacks there (its flash module on `SPMCSR`); `SLEEP` and the wake-up, where it would otherwise skip up to 1000 cycles and miss edges; an external level on a pin with pull-up (the button, the jumpers), set with `AVR_IOCTL_IOPORT_SET_EXTERNAL`, since a raised level alone is overwritten by the pull-up at every `DDR` or `PORT` write; the EEPROM content before the start, as a programmer leaves it (`avr_slave_eeprom_write()`).
- Not covered: the tinyAVR 0/1/2-series, which `simavr` cannot simulate. Their updater (`updater/hubus_updater_modern.c`) is built and size-checked for every device of the matrix; its protocol logic runs on the simulated bus with their layout (`test_update_boot_section`: updater in a boot section of 2 KB, application area behind it, no reset vector rule). The platform part, `NVMCTRL`, the start code without a C runtime, the clock, and the hand-over from the application, needs hardware.

### Daemon Tests

- `test_daemon_config`: YAML tree, configuration (defaults, every error class with line numbers, unknown keys, secrets), the modules directory and the plugins of hubusd, profiles that are no longer supported, JSON writer.
- `test_daemon_core`: `hubusd` against the bus simulation and a fake MQTT client: startup, discovery, events, commands and their validation, readback, polling, availability, statistics, line faults, reconnect, reload, shutdown, messages from a second thread, the functions of a module (instances, values, a function that hangs), and the module of the LED strip.
- `test_daemon_update`: firmware updates from a module, with `hubusd` against the real updater logic on the bus simulation: the trigger, progress, versions, a slave in its updater, a failed update that is repeated. The change of the MCID over MQTT: into a configured MCID and one outside the configuration, a slave in its updater, a refused change, invalid payloads.
- `test_module_worker`: the thread of a module on its own: the order of the calls, bounded queues, what the host functions accept, the time limit, and stopping a thread that hangs.
- `test_ledc`: the compiler from formulas to LED bytecode. What it produces runs in the interpreter of the slave.
- `hubusd_example_config`: `hubusd --check` on `daemon/hubusd.example.yaml`.
- `hubusd_e2e` (`test/tools/e2e.sh`): a real mosquitto broker, `hubusd_sim` (the daemon on a simulated relay, button, and temperature sensor), and `mosquitto_pub`/`mosquitto_sub` as clients. Skipped if mosquitto is not installed.
- Still open: an end-to-end test on hardware with Home Assistant ([roadmap](../TODO.md)).

## Documentation

The documentation lives in `docs/` ([index](README.md)) and follows the conventions described there: every fact in one place, links instead of repetition, the specification numbered by section.

- **Mermaid** diagrams are written directly into the Markdown (` ```mermaid ` blocks) and rendered by GitHub. Stick to flowcharts, sequence diagrams, and state diagrams, which every renderer supports.
- **Timing diagrams** use PlantUML, because Mermaid has no timing diagram. Each one is a source file `docs/diagrams/<name>.puml`; the rendered `docs/diagrams/<name>.svg` is committed next to it and embedded in the Markdown as an image.
- `sh tools/build-docs.sh` renders every PlantUML diagram to its SVG, renders every Mermaid diagram (to check it), and checks every relative link and `#anchor` in all Markdown files (`tools/check-docs-links.py`). Run it after every documentation change and commit the updated SVGs.
- The CI runs `sh tools/build-docs.sh --check`, which also fails if a committed SVG does not match its source.
- The tools are installed in fixed versions by `sh tools/setup-docs.sh` into `~/.local/share/hubus-docs` (`HUBUS_DOCS_TOOLS` overrides it): the PlantUML jar and mermaid-cli with its headless browser. Java, Node.js, and the browser libraries come from `tools/setup-ubuntu.sh`.
