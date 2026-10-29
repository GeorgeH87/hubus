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
