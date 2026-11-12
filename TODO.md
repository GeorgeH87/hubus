# TODO

Implementation roadmap for HUBUS v0. The specification is complete except for values that must be measured on hardware ([open issues](docs/project/open-issues.md)).

## 1. Development Environment

- [x] Set up WSL2 with Ubuntu on the development PC ([development](docs/development.md)).
- [x] Install host toolchain: `gcc`, `cmake`, `make`/`ninja`.
- [x] Install AVR toolchain: `gcc-avr`, `avr-libc`, `avrdude`, `simavr`.
- [x] Verify tinyAVR 0/1/2-series support of the AVR toolchain.
- [x] VS Code with the WSL extension.
- [x] Documentation tools: PlantUML for timing diagrams, mermaid-cli and a link check (`tools/build-docs.sh`).

## 2. Project Skeleton

- [x] Create directory structure:
  - `core/`: hardware-independent protocol code (shared by master, slave, and tests)
  - `slave/`: slave library with HALs for classic tinyAVR and tinyAVR 0/1/2-series
  - `master/`: Raspberry Pi master with `libgpiod` HAL
  - `test/`: unit tests and bus simulation
- [x] CMake build with presets `host` (tests, simulation, master) and `avr` (slave firmware via toolchain file).
- [x] Add `.gitignore`.
- [x] Integrate Unity as test framework.
- [x] Choose and add a license. MIT.

## 3. Protocol Core (`core/`)

- [x] CRC-8/AUTOSAR, incremental byte-wise API; test against check value `0xDF`.
- [x] Frame codec: 16-bit header encode/decode (`TYPE`, `TARGET_LEN`/`MORE`+`GEN`, `MCID`, `LEN`).
- [x] Target-mask prefix match, MCID `0` reserved.
- [x] Arbitration helpers: pending mask derivation from handoff slots, last-in-first-out mask stack.
- [x] `GEN` evaluation for the master (normal, duplicate, wrap, gap, baseline).
- [x] Slave state machine as pure logic: input = line event (`CLK` edge, START, STOP, RESET) and `DATA` level, output = `DATA` drive.
- [x] Unit tests for every module, built with sanitizers (`-fsanitize=address,undefined`).

## 4. Bus Simulation (`test/`)

- [x] Virtual open-drain wired-AND `DATA` line and master-driven `CLK`.
- [x] Simulated master and N simulated slaves using the real core code.
- [x] Scenarios: startup + RESET, idle event handshake, full burst with handoff slots, `REQUEST_INFO`, `REQUEST_DATA`, `REQUEST_STATE`, `SEND_COMMAND`, `STATUS_CAPTURE`, fragmented transfer with `MORE`.
- [x] Fault injection: bit flips, lost/extra clock edges, lost STOP, stuck line, duplicate MCID, event signal racing START.
- [x] Verify all accepted rare cases recover as specified.

## 5. Slave Library (`slave/`)

- [x] HAL interface: pin read, `DATA` pull-low/release, interrupt setup for `CLK` and `DATA`.
- [x] HAL for classic tinyAVR (pin change interrupts, register names per device).
- [x] HAL for tinyAVR 0/1/2-series (`PORTx`/`VPORTx`, `PINnCTRL`).
- [x] Automatic HAL selection from the compiler device macro.
- [x] Public API: init, event buffer write with `MORE`, `STATUS_CAPTURE` callback, command callback, static `SLAVE_INFO`, live state.
- [x] Compile-time buffer sizes; ignore oversized commands.
- [x] Event buffer and state as independent build options.
- [x] Protect buffer writes against the ISR (atomic access, refuse writes during transmission).
- [x] Speed class in `SLAVE_INFO` byte 0 (`speed_class` in the configuration, [§1.5](docs/spec/01-physical-layer.md#15-speed-classes)).
- [x] Blind sections after a command: `has_blind_time`, `hubus_slave_blind_begin()`/`hubus_slave_blind_end()` with resynchronization, waiting and deferred readback in hubusd ([§7.8](docs/spec/07-slave.md#78-blind-sections), [§6.9](docs/spec/06-operation.md#69-blind-time-after-a-command)).
- [x] Flash/RAM budget for the ATtiny85 reference build, checked with `avr-size` ([slave library](docs/slave-library.md#optional-parts)).
- [x] Run in simavr against the real master and measure ISR cycles, edge response, and stack depth (`test/avr/`, [AVR tests](docs/development.md#avr-tests); input for `T_CLK_LOW`, `T_COND`, `T_TURNAROUND`, and the buffer sizes of 128 B RAM devices, [open issues](docs/project/open-issues.md)).
- [x] Run the examples in simavr as well (`test_avr_examples`: sleep in the main loop, button, ADC) and the updater (`test_avr_update`: `SPM`, EEPROM, watchdog reset), [AVR tests](docs/development.md#avr-tests).
- [x] Remove the old ATtiny85-only implementation.

## 6. Master (`master/`)

- [x] HAL interface: set `CLK`, pull/release/read `DATA`, wait.
- [x] `libgpiod` HAL: chip selection by label, `DATA` as open-drain line. Implemented for the v2 API and compile-tested on Ubuntu 26.04 (libgpiod 2.2.1); not run on a Raspberry Pi yet, v1 support is still open ([H2](docs/project/open-issues.md#h2-master-clock-implementation)).
- [x] Line conditions: START, STOP, RESET; startup sequence; error recovery.
- [x] Transactions: `REQUEST_STATE`/`REQUEST_INFO`/`REQUEST_DATA` (exact, or group with arbitration and handoff slots), `SEND_COMMAND`.
- [x] Burst scheduling with pending mask stack and retry budget.
- [x] Idle event handshake detection.
- [x] `GEN` duplicate/gap handling, fragment reassembly hooks for applications.
- [x] Commissioning scan (group `REQUEST_INFO`) and duplicate MCID heuristic.
- [x] Configurable timing minimums (for the slowest slave on the bus).
- [x] Real-time priority option for the clock thread.
- [x] Derive the timing minimums from the speed classes of the known slaves: raised to `R` of the slowest plus `t_class_margin_ns`, never below the configuration; hubusd warns and shows the timing in `hubus/master/state` ([master library](docs/master-library.md#speed-classes), [hubusd](docs/daemon.md#speed-classes)).
- [ ] Later, if measurements show a gain: clock the rest of a frame faster once slower slaves have rejected its prefix ([open issues](docs/project/open-issues.md#speed-classes-on-the-master)).

## 7. Examples

- [x] Slave examples for every combination of event buffer and state: switch, relay, sensor, passive sensor, relay without feedback. Run on simulated ATtiny85 devices (`test_avr_examples`); not run on hardware.
- [x] Master example application (scan, burst, state, info, commands). Compiles; not run on a Raspberry Pi yet.
- [x] LED strip: fixed frames in a blind section, and animations from bytecode the master sends, shown in an interruptible section. Run on simulated ATtiny85 devices with the WS2812B signal decoded; not run on hardware.
- [x] LED strip on the master: formulas compiled to bytecode and sent by its module ([section 12](#12-master-modules-and-hardware-identifier)).

## 8. Continuous Integration

- [x] GitHub Actions: host build, unit tests, bus simulation, mutation check.
- [x] Slave firmware on simulated ATtiny85 devices in the host job (`test_avr_slave`, simavr).
- [x] AVR build matrix, also without the event buffer and without both optional parts.
- [x] Fail the build if the ATtiny85 reference build exceeds its flash/RAM budget.
- [x] Documentation check: diagrams render, SVGs up to date, links resolve.

## 9. Hardware Bring-Up

- [ ] Get an ISP programmer (USBasp or Arduino as ISP) and a UPDI programmer for 0/1/2-series devices.
- [ ] Get a logic analyzer (8 channels) and install PulseView/sigrok.
- [ ] Breadboard setup: Raspberry Pi + ATtiny85 slaves at 3.3 V, `DATA` pull-up to 3.3 V.
- [ ] Verify ATtiny input thresholds at 3.3 V against the datasheet.
- [ ] Measure clock rate and jitter on Pi 3, Pi 4, and Pi 5 ([H2](docs/project/open-issues.md#h2-master-clock-implementation)).
- [ ] Determine concrete timing values and the pull-up value; update [§1.4](docs/spec/01-physical-layer.md#14-timing-parameters) ([H3](docs/project/open-issues.md#h3-concrete-timing-values)).
- [ ] Multi-node test with mixed device families (`v0-multi` milestone).
- [ ] Firmware update on an ATtiny85: install the updater, load an application over the bus, interrupt an update ([open issues](docs/project/open-issues.md#firmware-update)).
- [ ] Firmware update on an ATtiny1614 (UPDI): fuses `BOOTEND`/`APPEND`, the updater in the boot section, an application behind it, the hand-over and the clock of the updater, an interrupted update, `UPDATE_SET_MCID` ([slave library](docs/slave-library.md#tinyavr-012-series)).
- [ ] Optional: sigrok protocol decoder for HUBUS.

## 10. Master Daemon (`hubusd`, MQTT)

Documentation: [hubusd](docs/daemon.md).

- [x] Configuration in YAML; unknown slaves are ignored and reported in the master state.
- [x] Daemon core: bus thread, command queue, statistics, availability tracking, tested against the bus simulation with a fake MQTT client.
- [x] Device profiles (relay, switch, temperature, custom) and Home Assistant discovery (replaced by the entities of the modules, section 12).
- [x] Configuration file parser with validation (line numbers, unknown keys, secrets).
- [x] `libmosquitto` binding, reconnect, last will, retained state; end-to-end test with a real broker on the simulated bus.
- [x] Readback from the cache for slaves that cannot be read back; periodic state reads (`poll_interval_s`).
- [x] systemd unit, installation notes.
- [ ] End-to-end test on hardware (Raspberry Pi, relay ATtiny, button ATtiny) and Home Assistant.

## 11. Firmware Update

- [x] Specification: update opcodes, updater information, info page ([§6.8](docs/spec/06-operation.md#68-firmware-update)).
- [x] Slave library option `HUBUS_ENABLE_UPDATE`: hand-over to the updater.
- [x] Updater for classic tinyAVR (ATtiny85, 2 KB at the end of flash).
- [x] Master: `hubus_master_update()`, Intel HEX reader, `update` command of the master example.
- [x] Bus simulation test with bit errors and an interrupted update.
- [x] Updater for the tinyAVR 0/1/2-series (`updater/hubus_updater_modern.c`): the boot section at 0 (`HUBUS_UPDATER_BOOT_SIZE`, default 2 KB, fuse `BOOTEND`), the application area behind it, no reset vector rule; flash and EEPROM through `NVMCTRL`, no C runtime, with `UPDATE_SET_MCID` (2010 of 2048 bytes on the ATtiny1614, 2020 on the ATtiny3216). Built and size-checked for every 0/1/2 device of 8 KB and more, with an application linked behind it (`hubus_size_reference_update`); its layout on the simulated bus (`test_update_boot_section`). The master checks the start of a HEX image against the application area (`HUBUS_UPD_REASON_WRONG_START`). Not run on hardware or a simulator (`simavr` has no 0/1/2 cores), [slave library](docs/slave-library.md#tinyavr-012-series).
- [x] Updates from hubusd: the image comes with the module, MQTT only triggers it ([hubusd](docs/daemon.md#firmware-updates)). The bus stands still meanwhile; a version in steps, so that other slaves are served, is open.

## 12. Master Modules and Hardware Identifier

Specification: [§4.7](docs/spec/04-frames.md#47-slave-info-slave_info), [§6.10](docs/spec/06-operation.md#610-master-modules).

- [x] Specification and glossary.
- [x] Central [table of HWID types](docs/hwid-types.md).
- [x] Slave library and core: `SLAVE_INFO` byte 0 and HWID, short and full form, optional serial number from RAM; no capability bits, command size, or blind class on the bus.
- [x] Updater: HWID type `0xFF` with the updater information, including its command size.
- [x] Typed module interface, validation, and selection by match rules (`master/hubus_module.h`, [master library](docs/master-library.md#master-modules)).
- [x] Master library: firmware update by the HWID.
- [x] hubusd: modules selected by the HWID; command size, state, status capture, and blind times per opcode from the module; slaves without a module get no commands.
- [x] Examples and tests.
- [x] Load modules as plugins (`modules.dir` of hubusd, `master/hubus_module_loader.h`): checks before use, name and rule conflicts reported at load time, files that others can write refused. The modules of the examples stay linked into hubusd.
- [x] hubusd: topics and discovery from the entities of the module (`hubus/slave/<n>/<key>/state` and `/set`); the profiles are gone.
- [x] Functions of a module (`create`, `destroy`, `on_frame`, `on_set`) on one thread per module, with a time limit per call ([hubusd](docs/daemon.md#module-functions)).
- [x] Entities of the kind `light` and `text`, and switches without fixed commands, through `on_set`, with discovery.
- [x] LED strip as a module: lamp, button, and animation formulas compiled to bytecode, sent as `PROG_WRITE` / `PROG_RUN` ([LED strip](docs/example-ledstrip.md#master-side)).
- [x] Reload modules on `SIGHUP`: an unchanged plugin stays with its thread and instances, a replaced one becomes a new module once no function of the old one runs, and a reload that would leave a slave without a module changes nothing.
- [x] Master example with modules (`--modules`): it names the module of a slave, and `hubus_master_update()` tells an application without update support (`HUBUS_UPD_REASON_NO_SUPPORT`) from a missing updater.
- [x] Example plugin in the build (`examples/plugin-dimmer`), installed to `lib/hubus/modules`, with notes for 32 and 64 bit Raspberry Pi OS.

Open:

- [ ] **Hardware test**: plugins, entities, and discovery on a Raspberry Pi with Home Assistant (together with the end-to-end test in section 10).

## 13. Assigning the MCID

Decisions: The library is not tied to a controller. It offers functions that the developer of a firmware selects (build options, as with the event buffer, the state, and `HUBUS_ENABLE_UPDATE`); nothing is prescribed, everything else is a recommendation. A firmware for the ATtiny85 may leave out the update, and with it the change of the MCID, just as any other firmware may include it. The source of the MCID is the choice of the slave application (EEPROM, GPIOs, a constant); the core only takes `cfg->mcid`. The **recommended way** is to set it when flashing: one image for all devices, one EEPROM byte per device (the last one, read by the updater). It is a recommendation, not a requirement; without it the default of the firmware applies and collisions are accepted. Where the update is built in, the MCID can in addition be changed over the bus, in the update mode only, if that part is selected too ([slave library](docs/slave-library.md#assigning-the-mcid), [decisions](docs/project/decisions.md)).

Flashing:

- [x] Every example takes its MCID from the last EEPROM byte, with its old MCID as the default (`examples/`, `test/avr/firmware/fw_slave.c`); the MCID is a parameter of the flash script, written together with the image in one avrdude call. Run on simulated ATtiny85 devices (`test_avr_examples`: two relays from one image, invalid bytes).
- [x] `tools/flash-avr.sh [-m mcu] [-c programmer] [-P port] [-n] image.hex [mcid]`: a one-byte Intel HEX image for the EEPROM, the offset of the last byte from avr-gcc (also for the 0/1/2-series), `-` for the MCID alone; without an MCID it says that the default applies. Tried with `-n` only, not with a programmer.
- [x] Slave library: `hubus_slave_mcid_from_eeprom(fallback)` and `hubus_slave_mcid_or()` in `slave/hubus_slave.h`: the last EEPROM byte as the updater reads it, range check, fallback; on the 0/1/2-series the offset of the mapped EEPROM.
- [x] `example_relay_simple_pins`: the simple relay with its MCID from two jumpers (`0x20` plus PB1, PB4); the updater stores what the application hands over, and a change over the bus does not last, which the master undoes and reports.

Change over the bus (update mode):

- [x] Specification [§6.8](docs/spec/06-operation.md#mcid): `UPDATE_SET_MCID` (`0x05`, `mcid ~mcid`, exact addressing only), flag bit 1 in the updater information. Takes effect at once (from the next transaction), answers nothing, leaves the state unchanged; the master asks the new MCID.
- [x] The state after the change, documented: the updater keeps the MCID in the last EEPROM byte; an application with its own source ignores it, so the master checks under which MCID the application answers and undoes the change otherwise.
- [x] Updater: `HUBUS_UPD_SET_MCID` (CMake `HUBUS_UPDATER_SET_MCID`, on for the ATtiny85 reference updater), range check, `eeprom_update_byte` through the platform function `hubus_upd_store_mcid()`, the new MCID in the running core. 60 bytes: 2040 instead of 1980 of 2048; the build also produces `hubus_updater_no_set_mcid` and checks both.
- [x] The option is in the hardware-independent part of the updater (`updater/hubus_updater.c`); another updater only provides `hubus_upd_store_mcid()`, and the flag tells the master. The updater for the 0/1/2-series has it too (section 11).
- [x] Master library: `hubus_master_set_mcid(m, mcid, new_mcid, modules, n_modules, &res)`: refuses a taken MCID, enters the update mode, repeats the opcode until the updater answers to the new MCID, leaves the update mode under the new one; reasons `MCID_TAKEN`, `NO_SET_MCID`, `MCID_NOT_SET`, `MCID_NOT_KEPT` (with the change undone), `NO_SUPPORT` from the module.
- [x] Master example: command `mcid <old> <new>`. Compiles; not run on a Raspberry Pi.
- [x] hubusd: `hubus/slave/<n>/mcid` with the new MCID, result on `hubus/master/mcid`. It does not write its configuration: a configured new MCID goes into operation with its module, another one is reported as unknown, and the log says what the configuration has to follow; the old MCID goes offline.
- [x] Tests: `test_update` and `test_update_no_set_mcid` on the bus simulation (from an application and from the updater, taken MCID, the checks of the updater, bit errors, power loss in update mode, an application with its own MCID, a module without update support, an updater without the option), `test_daemon_update` (MQTT), `test_avr_update` on a simulated ATtiny85 (the MCID from the programmer, a fresh device that gets its MCID over the bus, the running switch that moves and keeps it after a power cycle and an update).
- [ ] Hardware: change the MCID on an ATtiny85 and survive an interrupted change (together with the firmware update test in section 9); the flash script with a real programmer and `EESAVE`.

Documentation:

- [x] The MCID parts rewritten: [slave library](docs/slave-library.md#assigning-the-mcid), [§6.8](docs/spec/06-operation.md#mcid), [§7.2](docs/spec/07-slave.md#72-mcid), [master library](docs/master-library.md#firmware-update), [hubusd](docs/daemon.md#changing-the-mcid), [development](docs/development.md#avr-tests), the header of `updater/hubus_updater_avr.c`.
- [x] Fuse settings in one place ([slave library](docs/slave-library.md#fuses)): `EESAVE` (now in the recommended `hfuse` `0xD5`), `SELFPRGEN`, brown-out detection, `WDTON`; a flash write erases the chip, and without `EESAVE` the MCID with it.
- [x] The firmware update over the bus does not touch the EEPROM: the MCID survives it (spec, slave library, hubusd).
- [x] Glossary, [open issues](docs/project/open-issues.md#mcid-assignment) (no automatic enumeration without a unique serial number), [decisions](docs/project/decisions.md).

## 14. Sensor Modules for the ATtiny85

Three new devices, each with slave firmware (always with the update option), a master module, an entry in the [HWID types](docs/hwid-types.md), tests, and a page like the [LED strip](docs/example-ledstrip.md). Decisions: HWID types `0x86` (DHT11), `0x87` (HC-SR04), `0x88` (HC-SR501); one variant each, built with the updater; the distance threshold is sent by the master, with a default of 30 cm.

Free pins next to `CLK` (PB2) and `DATA` (PB3): PB0, PB1, PB4 (PB5 is reset).

- [x] Entity kind `number` (`HUBUS_COMPONENT_NUMBER`, `min`/`max`/`step`, value through `on_set`, discovery as a box input; module API version 2).
- [x] LED strip and push button: the build with the updater fits below the info page, 6076 of 6080 bytes; the AVR build fails once it no longer fits (`example_ledstrip_update_size`, [sizes](docs/example-ledstrip.md#sizes)). HWID variant `2` with the module `ledstrip-update`; updated on a simulated ATtiny85 while an animation runs (`test_avr_update`), the module on the simulated bus (`test_daemon_core`); not run on hardware.
- [x] DHT11 (`0x86`): temperature and humidity as state, read at most once per second; the 40-bit read in an interruptible section that is repeated when the bus needs an edge. Module with two sensors. Test with a simulated DHT11 (pulse widths, bad checksum, absent sensor). Run on a simulated ATtiny85 with a model of the sensor (`test_avr_examples`); not run on hardware, [description](docs/example-dht11.md).
- [x] HC-SR04 (`0x87`): echo width measured on PB1 (trigger PB0), distance as state, an event when it falls below the threshold (and when it leaves it, with hysteresis). Command to set the threshold; the slave reports "no threshold" after power-up and the master sends the stored value, default 30 cm. Module with a distance sensor and a `number` entity. The echo is 5 V: divider or a 3.3 V sensor. Test with a simulated echo. Run on a simulated ATtiny85 with a model of the module (`test_avr_examples`), the module on the simulated bus (`test_daemon_core`); not run on hardware, [description](docs/example-distance.md).
- [x] HC-SR501 (`0x88`): event buffer only (1 motion, 0 cleared), no state; warm-up of the PIR after power-up. Module: `binary_sensor` with the device class `motion`. Test on a simulated ATtiny85. Run on a simulated ATtiny85 (`test_avr_examples`); not run on hardware, [description](docs/example-motion.md).
- [x] Tables, daemon documentation, and size checks for the new firmware (`example_<name>_size` in the AVR build, which the CI builds).
- [ ] Hardware test together with section 9.
