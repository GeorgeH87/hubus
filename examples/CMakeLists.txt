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

    # hubus_example(name source events state [tx_buf_size])
    function(hubus_example name source events state)
        if(ARGC GREATER 4)
            set(tx_buf ${ARGV4})
        else()
            set(tx_buf ${HUBUS_TX_BUF_SIZE})
        endif()
        add_executable(example_${name} ${source} ${HUBUS_EXAMPLE_LIB_SOURCES})
        target_include_directories(example_${name} PRIVATE ${PROJECT_SOURCE_DIR}/core ${PROJECT_SOURCE_DIR}/slave)
        target_compile_definitions(example_${name} PRIVATE
            HUBUS_CLK_PORT=${clk_port} HUBUS_CLK_PIN=${clk_pin}
            HUBUS_DATA_PORT=${data_port} HUBUS_DATA_PIN=${data_pin}
            HUBUS_TX_BUF_SIZE=${tx_buf} HUBUS_RX_BUF_SIZE=${HUBUS_RX_BUF_SIZE}
            HUBUS_ENABLE_EVENTS=${events} HUBUS_ENABLE_STATE=${state})
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
    # 2-byte event buffer: the RAM goes to the LED frame buffer
    hubus_example(ledstrip        slave-ledstrip/ledstrip_example.c              1      1  2)
endif()
