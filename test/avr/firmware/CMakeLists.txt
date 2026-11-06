# Test firmware for the AVR simulation tests, built with the AVR toolchain as an external project of the host build
# (test/avr/CMakeLists.txt). HUBUS_ROOT is the repository root; HUBUS_WERROR follows the host build.
cmake_minimum_required(VERSION 3.21)
project(hubus_avr_test_firmware LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_EXTENSIONS ON)

set(HUBUS_FW_LIB_SOURCES
    ${HUBUS_ROOT}/core/hubus_slave_core.c
    ${HUBUS_ROOT}/core/hubus_crc.c
    ${HUBUS_ROOT}/core/hubus_frame.c
    ${HUBUS_ROOT}/slave/hubus_slave.c)

# hubus_fw(name mcid events state): the test firmware with the library and core compiled for these options.
function(hubus_fw name mcid events state)
    add_executable(${name} fw_slave.c ${HUBUS_FW_LIB_SOURCES})
    target_include_directories(${name} PRIVATE ${HUBUS_ROOT}/core ${HUBUS_ROOT}/slave)
    target_compile_definitions(${name} PRIVATE
        FW_MCID=${mcid} HUBUS_ENABLE_EVENTS=${events} HUBUS_ENABLE_STATE=${state})
    target_compile_options(${name} PRIVATE -Wall -Wextra $<$<BOOL:${HUBUS_WERROR}>:-Werror>)
    set_target_properties(${name} PROPERTIES SUFFIX ".elf")
endfunction()

#        name             mcid events state
hubus_fw(fw_slave_5       5    1      1)
hubus_fw(fw_slave_6       6    1      1)
hubus_fw(fw_passive_9     9    0      1)
hubus_fw(fw_command_10    10   0      0)

# The LED strip example (MCID 11) as it is shipped, with its WS2812B output on PB0.
add_executable(fw_ledstrip ${HUBUS_ROOT}/examples/slave-ledstrip/ledstrip_example.c
    ${HUBUS_ROOT}/examples/slave-ledstrip/ledvm.c ${HUBUS_FW_LIB_SOURCES})
target_include_directories(fw_ledstrip PRIVATE ${HUBUS_ROOT}/core ${HUBUS_ROOT}/slave)
target_compile_definitions(fw_ledstrip PRIVATE HUBUS_ENABLE_EVENTS=1 HUBUS_ENABLE_STATE=1 HUBUS_TX_BUF_SIZE=3)
target_compile_options(fw_ledstrip PRIVATE -Wall -Wextra $<$<BOOL:${HUBUS_WERROR}>:-Werror>)
set_target_properties(fw_ledstrip PROPERTIES SUFFIX ".elf")

# The slave examples as they are shipped (examples/CMakeLists.txt), with the default buffers of the library.
# hubus_fw_example(name source events state [definitions...])
function(hubus_fw_example name source events state)
    add_executable(${name} ${HUBUS_ROOT}/examples/${source} ${HUBUS_FW_LIB_SOURCES})
    target_include_directories(${name} PRIVATE ${HUBUS_ROOT}/core ${HUBUS_ROOT}/slave)
    target_compile_definitions(${name} PRIVATE HUBUS_ENABLE_EVENTS=${events} HUBUS_ENABLE_STATE=${state} ${ARGN})
    target_compile_options(${name} PRIVATE -Wall -Wextra $<$<BOOL:${HUBUS_WERROR}>:-Werror>)
    set_target_properties(${name} PROPERTIES SUFFIX ".elf")
    add_custom_command(TARGET ${name} POST_BUILD
        COMMAND ${AVR_OBJCOPY} -O ihex -R .eeprom $<TARGET_FILE:${name}> $<TARGET_FILE_DIR:${name}>/${name}.hex)
endfunction()

#                name               source                                         events state
hubus_fw_example(ex_relay           slave-relay/relay_example.c                    1      1)
hubus_fw_example(ex_switch          slave-switch/switch_example.c                  1      1)
hubus_fw_example(ex_sensor          slave-sensor/sensor_example.c                  1      0)
hubus_fw_example(ex_sensor_passive  slave-sensor-passive/sensor_passive_example.c  0      1)
hubus_fw_example(ex_relay_simple    slave-relay-simple/relay_simple_example.c      0      0)

# Firmware update (updater/CMakeLists.txt): the switch with update support, loaded through the updater, and the
# updater itself. The updater is built with the MCID of the switch, so that the master finds the application under
# the MCID it updated; fw_updater keeps the default MCID 63.
set(HUBUS_FW_UPDATER_ADDR 0x1800)
hubus_fw_example(ex_switch_update   slave-switch/switch_example.c                  1      1
                 HUBUS_ENABLE_UPDATE=1 HUBUS_UPDATER_ADDR=${HUBUS_FW_UPDATER_ADDR})

# hubus_fw_updater(name mcid)
function(hubus_fw_updater name mcid)
    add_executable(${name} ${HUBUS_ROOT}/updater/hubus_updater_avr.c ${HUBUS_ROOT}/updater/hubus_updater.c
        ${HUBUS_ROOT}/core/hubus_slave_core.c ${HUBUS_ROOT}/core/hubus_crc.c ${HUBUS_ROOT}/core/hubus_frame.c)
    target_include_directories(${name} PRIVATE ${HUBUS_ROOT}/updater ${HUBUS_ROOT}/core ${HUBUS_ROOT}/slave)
    target_compile_definitions(${name} PRIVATE
        HUBUS_RX_BUF_SIZE=31 HUBUS_ENABLE_EVENTS=0 HUBUS_ENABLE_STATE=1 HUBUS_UPDATER=1
        HUBUS_UPD_ADDR=${HUBUS_FW_UPDATER_ADDR} HUBUS_UPDATER_MCID=${mcid})
    target_compile_options(${name} PRIVATE -Wall -Wextra $<$<BOOL:${HUBUS_WERROR}>:-Werror> -flto)
    target_link_options(${name} PRIVATE -flto -Wl,--section-start=.text=${HUBUS_FW_UPDATER_ADDR}
        -Wl,--section-start=.hubus_reset=0 -Wl,--undefined=hubus_reset_vector)
    set_target_properties(${name} PROPERTIES SUFFIX ".elf")
    # The reset vector at address 0 is a section of its own: the tests load the HEX file, which holds both.
    add_custom_command(TARGET ${name} POST_BUILD
        COMMAND ${AVR_OBJCOPY} -O ihex -R .eeprom $<TARGET_FILE:${name}> $<TARGET_FILE_DIR:${name}>/${name}.hex)
endfunction()

hubus_fw_updater(fw_updater_5 5)
hubus_fw_updater(fw_updater   63)
