# Example slave firmware for the ATtiny85 (CLK = PB2, DATA = PB3). The CI
# device matrix builds the size reference firmware for the other devices.
#
# Each example builds the slave library itself with the optional parts it uses
# (docs/spec/07-slave.md section 7.4), independent of the options of this
# build directory; buffers and pins follow this build.
if(HUBUS_AVR AND HUBUS_AVR_MCU STREQUAL "attiny85")
    set(HUBUS_EXAMPLE_LIB_SOURCES
        ${PROJECT_SOURCE_DIR}/core/hubus_slave_core.c
        ${PROJECT_SOURCE_DIR}/core/hubus_crc.c
        ${PROJECT_SOURCE_DIR}/core/hubus_frame.c
        ${PROJECT_SOURCE_DIR}/slave/hubus_slave.c)

    # hubus_example(name source events state [TX_BUF size] [extra definitions...])
    function(hubus_example name source events state)
        cmake_parse_arguments(PARSE_ARGV 4 ex "" "TX_BUF" "")
        if(NOT ex_TX_BUF)
            set(ex_TX_BUF ${HUBUS_TX_BUF_SIZE})
        endif()
        add_executable(example_${name} ${source} ${HUBUS_EXAMPLE_LIB_SOURCES})
        target_include_directories(example_${name} PRIVATE ${PROJECT_SOURCE_DIR}/core ${PROJECT_SOURCE_DIR}/slave)
        target_compile_definitions(example_${name} PRIVATE
            HUBUS_CLK_PORT=${clk_port} HUBUS_CLK_PIN=${clk_pin}
            HUBUS_DATA_PORT=${data_port} HUBUS_DATA_PIN=${data_pin}
            HUBUS_TX_BUF_SIZE=${ex_TX_BUF} HUBUS_RX_BUF_SIZE=${HUBUS_RX_BUF_SIZE}
            HUBUS_ENABLE_EVENTS=${events} HUBUS_ENABLE_STATE=${state} ${ex_UNPARSED_ARGUMENTS})
        target_compile_options(example_${name} PRIVATE -Wall -Wextra)
        set_target_properties(example_${name} PROPERTIES SUFFIX ".elf")
        add_custom_command(TARGET example_${name} POST_BUILD
            COMMAND ${AVR_OBJCOPY} -O ihex -R .eeprom $<TARGET_FILE:example_${name}> $<TARGET_FILE_DIR:example_${name}>/example_${name}.hex
            COMMAND ${AVR_SIZE} $<TARGET_FILE:example_${name}>)
    endfunction()

    #             name            source                                         events state
    hubus_example(relay           slave-relay/relay_example.c                    1      1)
    hubus_example(switch          slave-switch/switch_example.c                  1      1)
    hubus_example(sensor          slave-sensor/sensor_example.c                  1      0)
    hubus_example(sensor_passive  slave-sensor-passive/sensor_passive_example.c  0      1)
    hubus_example(relay_simple    slave-relay-simple/relay_simple_example.c      0      0)

    # The switch with firmware update: installed together with the updater, and loadable through it. It must end
    # before the info page, the last page in front of the updater.
    hubus_example(switch_update   slave-switch/switch_example.c                  1      1
                  HUBUS_ENABLE_UPDATE=1 HUBUS_UPDATER_ADDR=${HUBUS_UPDATER_ADDR})
    math(EXPR app_space "${HUBUS_UPDATER_ADDR} - 64")
    add_custom_target(example_switch_update_size ALL
        COMMAND ${CMAKE_COMMAND} -DSIZE_TOOL=${AVR_SIZE} -DELF=$<TARGET_FILE:example_switch_update>
                -DMCU=${HUBUS_AVR_MCU} -DMAX_FLASH=${app_space} -DMAX_RAM=400
                -P ${PROJECT_SOURCE_DIR}/cmake/check-size.cmake
        DEPENDS example_switch_update
        COMMENT "Checking that the application fits below the updater")

    # 3-byte event buffer: the RAM goes to the LED frame buffer and the animation program
    hubus_example(ledstrip        "slave-ledstrip/ledstrip_example.c;slave-ledstrip/ledvm.c" 1 1 TX_BUF 3)
    add_custom_target(example_ledstrip_size ALL
        COMMAND ${CMAKE_COMMAND} -DSIZE_TOOL=${AVR_SIZE} -DELF=$<TARGET_FILE:example_ledstrip>
                -DMCU=${HUBUS_AVR_MCU} -DMAX_FLASH=8192 -DMAX_RAM=384
                -P ${PROJECT_SOURCE_DIR}/cmake/check-size.cmake
        DEPENDS example_ledstrip
        COMMENT "Checking that the LED strip leaves room for the stack")
endif()
