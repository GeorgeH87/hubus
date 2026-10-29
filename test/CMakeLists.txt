set(HUBUS_TEST_WARNINGS -Wall -Wextra -Wshadow)
if(HUBUS_WERROR)
    list(APPEND HUBUS_TEST_WARNINGS -Werror)
endif()

add_library(unity STATIC unity/unity.c)
target_include_directories(unity PUBLIC unity)
target_compile_options(unity PRIVATE -w) # third-party code

add_library(hubus_sim STATIC sim/hubus_sim.c)
target_include_directories(hubus_sim PUBLIC sim support)
target_link_libraries(hubus_sim PUBLIC hubus_master hubus_core)
target_compile_options(hubus_sim PRIVATE ${HUBUS_TEST_WARNINGS})

function(hubus_test name)
    if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${name}.c)
        return()
    endif()
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE unity hubus_sim ${ARGN})
    target_compile_options(${name} PRIVATE ${HUBUS_TEST_WARNINGS})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

hubus_test(test_crc)
hubus_test(test_frame)
hubus_test(test_arb)
hubus_test(test_gen)
hubus_test(test_slave_core)
hubus_test(test_master)
hubus_test(test_faults)
hubus_test(test_blind)
hubus_test(test_slave_lib hubus_slave_host)

# The animation bytecode of the LED strip example.
add_executable(test_ledvm test_ledvm.c ${PROJECT_SOURCE_DIR}/examples/slave-ledstrip/ledvm.c)
target_include_directories(test_ledvm PRIVATE ${PROJECT_SOURCE_DIR}/examples/slave-ledstrip)
target_link_libraries(test_ledvm PRIVATE unity hubus_core)
target_compile_options(test_ledvm PRIVATE ${HUBUS_TEST_WARNINGS})
add_test(NAME test_ledvm COMMAND test_ledvm)
set_tests_properties(test_ledvm PROPERTIES TIMEOUT 120)

# A test of the slave core in another build configuration: the core and the simulation are compiled again with the
# given definitions.
function(hubus_core_variant_test name source)
    if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${source})
        return()
    endif()
    add_executable(${name} ${source} ${HUBUS_CORE_SOURCES_ABS} sim/hubus_sim.c)
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/core ${PROJECT_SOURCE_DIR}/master sim support)
    target_compile_definitions(${name} PRIVATE ${ARGN})
    target_link_libraries(${name} PRIVATE unity)
    target_compile_options(${name} PRIVATE ${HUBUS_TEST_WARNINGS})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

# Small buffers: oversized commands must be ignored. REQUEST_STATE support is compiled out as well.
hubus_core_variant_test(test_small_buffers test_small_buffers.c HUBUS_RX_BUF_SIZE=4 HUBUS_TX_BUF_SIZE=6 HUBUS_ENABLE_STATE=0)
# Without the event buffer: a passive slave that is only read, and one that only executes commands.
hubus_core_variant_test(test_slave_passive test_slave_parts.c HUBUS_ENABLE_EVENTS=0 HUBUS_ENABLE_STATE=1)
hubus_core_variant_test(test_slave_command_only test_slave_parts.c HUBUS_ENABLE_EVENTS=0 HUBUS_ENABLE_STATE=0)

# Firmware update end to end: master library, updater logic, and an application core with update support on one
# simulated bus. The core and the master are compiled again for the updater configuration.
add_executable(test_update test_update.c support/update_app_core.c sim/hubus_sim.c
    ${HUBUS_CORE_SOURCES_ABS} ${PROJECT_SOURCE_DIR}/updater/hubus_updater.c
    ${PROJECT_SOURCE_DIR}/master/hubus_master.c ${PROJECT_SOURCE_DIR}/master/hubus_update.c)
target_include_directories(test_update PRIVATE ${PROJECT_SOURCE_DIR}/core ${PROJECT_SOURCE_DIR}/master
    ${PROJECT_SOURCE_DIR}/updater sim support)
target_compile_definitions(test_update PRIVATE
    HUBUS_ENABLE_EVENTS=0 HUBUS_ENABLE_STATE=1 HUBUS_UPDATER=1 HUBUS_UPD_ADDR=0x1800)
target_link_libraries(test_update PRIVATE unity)
target_compile_options(test_update PRIVATE ${HUBUS_TEST_WARNINGS})
add_test(NAME test_update COMMAND test_update)
set_tests_properties(test_update PROPERTIES TIMEOUT 120)

# The slave firmware on simulated AVR devices (simavr).
add_subdirectory(avr)

if(HUBUSD_LIB_AVAILABLE)
    hubus_test(test_daemon_config hubusd_lib)
    hubus_test(test_daemon_core hubusd_lib)
endif()

# Manual end-to-end tool: hubusd on a simulated bus with a relay, a button, and a temperature sensor.
if(TARGET hubusd_run)
    add_executable(hubusd_sim tools/hubusd_sim.c)
    target_link_libraries(hubusd_sim PRIVATE hubusd_run hubus_sim)
endif()

# The example configuration must stay valid.
if(TARGET hubusd)
    add_test(NAME hubusd_example_config COMMAND hubusd --check -c ${PROJECT_SOURCE_DIR}/daemon/hubusd.example.yaml)
endif()

# End-to-end test with a real MQTT broker (skipped if mosquitto is not installed).
find_program(MOSQUITTO_BROKER mosquitto)
find_program(MOSQUITTO_PUB mosquitto_pub)
find_program(MOSQUITTO_SUB mosquitto_sub)
if(TARGET hubusd_sim AND MOSQUITTO_BROKER AND MOSQUITTO_PUB AND MOSQUITTO_SUB)
    add_test(NAME hubusd_e2e COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tools/e2e.sh ${CMAKE_BINARY_DIR}
             WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(hubusd_e2e PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
endif()
