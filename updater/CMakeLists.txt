# Updater firmware (docs/spec/06-operation.md section 6.8) for the ATtiny85: the last 2 KB of flash. It brings its
# own copy of the protocol core, built as updater: no event buffer, a state, the update opcodes.
if(HUBUS_AVR_MCU STREQUAL "attiny85")
    set(HUBUS_UPDATER_ADDR 0x1800 CACHE STRING "Byte address of the updater; applications with update support use the same value")
    set(HUBUS_UPDATER_MCID 63 CACHE STRING "MCID of the updater before an application has stored its own in EEPROM")

    add_executable(hubus_updater
        hubus_updater_avr.c hubus_updater.c
        ${PROJECT_SOURCE_DIR}/core/hubus_slave_core.c
        ${PROJECT_SOURCE_DIR}/core/hubus_crc.c
        ${PROJECT_SOURCE_DIR}/core/hubus_frame.c)
    target_include_directories(hubus_updater PRIVATE . ${PROJECT_SOURCE_DIR}/core ${PROJECT_SOURCE_DIR}/slave)
    target_compile_definitions(hubus_updater PRIVATE
        HUBUS_CLK_PORT=${clk_port} HUBUS_CLK_PIN=${clk_pin}
        HUBUS_DATA_PORT=${data_port} HUBUS_DATA_PIN=${data_pin}
        HUBUS_RX_BUF_SIZE=31 HUBUS_ENABLE_EVENTS=0 HUBUS_ENABLE_STATE=1 HUBUS_UPDATER=1
        HUBUS_UPD_ADDR=${HUBUS_UPDATER_ADDR} HUBUS_UPDATER_MCID=${HUBUS_UPDATER_MCID})
    # Link-time optimisation brings the updater from about 2200 to about 2000 bytes, below the 2 KB it has.
    target_compile_options(hubus_updater PRIVATE ${HUBUS_WARNINGS} -flto)
    # .hubus_reset is the reset vector at address 0 that points to the updater (hubus_updater_avr.c).
    target_link_options(hubus_updater PRIVATE -flto -Wl,--section-start=.text=${HUBUS_UPDATER_ADDR}
        -Wl,--section-start=.hubus_reset=0 -Wl,--undefined=hubus_reset_vector)
    set_target_properties(hubus_updater PROPERTIES SUFFIX ".elf")

    # It must fit between its address and the end of flash.
    math(EXPR updater_space "8192 - ${HUBUS_UPDATER_ADDR}")
    add_custom_target(hubus_updater_size ALL
        COMMAND ${AVR_OBJCOPY} -O ihex -R .eeprom $<TARGET_FILE:hubus_updater> ${CMAKE_CURRENT_BINARY_DIR}/hubus_updater.hex
        COMMAND ${CMAKE_COMMAND} -DSIZE_TOOL=${AVR_SIZE} -DELF=$<TARGET_FILE:hubus_updater>
                -DMCU=${HUBUS_AVR_MCU} -DMAX_FLASH=${updater_space} -DMAX_RAM=400
                -P ${PROJECT_SOURCE_DIR}/cmake/check-size.cmake
        DEPENDS hubus_updater
        COMMENT "Checking the updater size")
endif()
