# Example slave firmware for the ATtiny85 (CLK = PB2, DATA = PB3). The CI
# device matrix builds the size reference firmware for the other devices.
if(HUBUS_AVR AND HUBUS_AVR_MCU STREQUAL "attiny85")
    foreach(example relay switch sensor)
        if(example STREQUAL "relay")
            set(source slave-relay/relay_example.c)
        elseif(example STREQUAL "switch")
            set(source slave-switch/switch_example.c)
        else()
            set(source slave-sensor/sensor_example.c)
        endif()
        add_executable(example_${example} ${source})
        target_link_libraries(example_${example} PRIVATE hubus_slave)
        target_compile_options(example_${example} PRIVATE -Wall -Wextra)
        set_target_properties(example_${example} PROPERTIES SUFFIX ".elf")
        add_custom_command(TARGET example_${example} POST_BUILD
            COMMAND ${AVR_OBJCOPY} -O ihex -R .eeprom $<TARGET_FILE:example_${example}> $<TARGET_FILE_DIR:example_${example}>/example_${example}.hex
            COMMAND ${AVR_SIZE} $<TARGET_FILE:example_${example}>)
    endforeach()
endif()
