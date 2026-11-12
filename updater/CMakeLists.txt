# Updater firmware (docs/spec/06-operation.md section 6.8). It brings its own copy of the protocol core, built as
# updater: no event buffer, a state, the update opcodes.
#
#   ATtiny85                          the last 2 KB of flash, application area from 0 (hubus_updater_avr.c)
#   tinyAVR 0/1/2-series, 8 KB and up the boot section at 0, application area behind it (hubus_updater_modern.c)
set(HUBUS_UPDATER_MCID 63 CACHE STRING "MCID of the updater while the last EEPROM byte holds none")
# UPDATE_SET_MCID: the 2 KB of the updater are reserved anyway, so the reference updater takes it.
option(HUBUS_UPDATER_SET_MCID "Updater takes UPDATE_SET_MCID (change of the MCID over the bus)" ON)

# hubus_updater_build(name set_mcid platform_source max_flash definitions link_options): the updater, and a check that
# it fits.
function(hubus_updater_build name set_mcid platform max_flash defs links)
    add_executable(${name}
        ${platform} hubus_updater.c
        ${PROJECT_SOURCE_DIR}/core/hubus_slave_core.c
        ${PROJECT_SOURCE_DIR}/core/hubus_crc.c
        ${PROJECT_SOURCE_DIR}/core/hubus_frame.c)
    target_include_directories(${name} PRIVATE . ${PROJECT_SOURCE_DIR}/core ${PROJECT_SOURCE_DIR}/slave)
    target_compile_definitions(${name} PRIVATE
        HUBUS_CLK_PORT=${clk_port} HUBUS_CLK_PIN=${clk_pin}
        HUBUS_DATA_PORT=${data_port} HUBUS_DATA_PIN=${data_pin}
        HUBUS_RX_BUF_SIZE=31 HUBUS_ENABLE_EVENTS=0 HUBUS_ENABLE_STATE=1 HUBUS_UPDATER=1
        HUBUS_UPDATER_MCID=${HUBUS_UPDATER_MCID} HUBUS_UPD_SET_MCID=${set_mcid} ${defs})
    # Link-time optimisation brings the updater from about 2200 to about 2000 bytes, below the 2 KB it has.
    target_compile_options(${name} PRIVATE ${HUBUS_WARNINGS} -flto)
    target_link_options(${name} PRIVATE -flto ${links})
    set_target_properties(${name} PROPERTIES SUFFIX ".elf")

    add_custom_target(${name}_size ALL
        COMMAND ${AVR_OBJCOPY} -O ihex -R .eeprom $<TARGET_FILE:${name}> ${CMAKE_CURRENT_BINARY_DIR}/${name}.hex
        COMMAND ${CMAKE_COMMAND} -DSIZE_TOOL=${AVR_SIZE} -DELF=$<TARGET_FILE:${name}>
                -DMCU=${HUBUS_AVR_MCU} -DMAX_FLASH=${max_flash} -DMAX_RAM=400 -DENFORCE=ON
                -P ${PROJECT_SOURCE_DIR}/cmake/check-size.cmake
        DEPENDS ${name}
        COMMENT "Checking the size of ${name} (UPDATE_SET_MCID: ${set_mcid})")
endfunction()

# hubus_updater_variants(platform max_flash definitions link_options): with UPDATE_SET_MCID, and the same without it,
# so that the build shows what it costs (not installed).
function(hubus_updater_variants platform max_flash defs links)
    if(HUBUS_UPDATER_SET_MCID)
        hubus_updater_build(hubus_updater 1 ${platform} ${max_flash} "${defs}" "${links}")
        hubus_updater_build(hubus_updater_no_set_mcid 0 ${platform} ${max_flash} "${defs}" "${links}")
    else()
        hubus_updater_build(hubus_updater 0 ${platform} ${max_flash} "${defs}" "${links}")
    endif()
endfunction()

if(HUBUS_AVR_MCU STREQUAL "attiny85")
    set(HUBUS_UPDATER_ADDR 0x1800 CACHE STRING "Byte address of the updater; applications with update support use the same value")
    # It must fit between its address and the end of flash.
    math(EXPR updater_space "8192 - ${HUBUS_UPDATER_ADDR}")
    # .hubus_reset is the reset vector at address 0 that points to the updater (hubus_updater_avr.c).
    hubus_updater_variants(hubus_updater_avr.c ${updater_space} "HUBUS_UPD_ADDR=${HUBUS_UPDATER_ADDR}"
        "-Wl,--section-start=.text=${HUBUS_UPDATER_ADDR};-Wl,--section-start=.hubus_reset=0;-Wl,--undefined=hubus_reset_vector")
elseif(HUBUS_AVR_MCU MATCHES "^attiny((8|16)(0[467]|1[467]|2[467])|32(1[67]|2[467]))$")
    # tinyAVR 0/1/2-series with 8, 16, or 32 KB of flash: the updater is the boot section.
    set(HUBUS_UPDATER_BOOT_SIZE 2048 CACHE STRING
        "Boot section of the tinyAVR 0/1/2-series in bytes (BOOTEND fuse times 256): the updater, the application behind it")
    if(HUBUS_AVR_MCU MATCHES "^attiny8")
        set(flash_size 8192)
    elseif(HUBUS_AVR_MCU MATCHES "^attiny16")
        set(flash_size 16384)
    else()
        set(flash_size 32768)
    endif()
    math(EXPR boot_check "${HUBUS_UPDATER_BOOT_SIZE} % 256")
    if(NOT boot_check EQUAL 0 OR HUBUS_UPDATER_BOOT_SIZE LESS 256 OR NOT HUBUS_UPDATER_BOOT_SIZE LESS flash_size)
        message(FATAL_ERROR "HUBUS_UPDATER_BOOT_SIZE must be a multiple of 256 below the flash size ${flash_size}")
    endif()
    math(EXPR app_size "${flash_size} - ${HUBUS_UPDATER_BOOT_SIZE}")
    math(EXPR bootend "${HUBUS_UPDATER_BOOT_SIZE} / 256")
    # ld reads the address of --section-start as hexadecimal, also without 0x
    math(EXPR app_start_hex "${HUBUS_UPDATER_BOOT_SIZE}" OUTPUT_FORMAT HEXADECIMAL)
    message(STATUS "Updater in the boot section: ${HUBUS_UPDATER_BOOT_SIZE} bytes, fuse BOOTEND = ${bootend}, APPEND = 0")
    # Without the C runtime and its vector table (hubus_updater_modern.c); -mrelax turns calls and jumps into the
    # short forms.
    hubus_updater_variants(hubus_updater_modern.c ${HUBUS_UPDATER_BOOT_SIZE}
        "HUBUS_UPD_ADDR=0;HUBUS_UPD_APP_START=${HUBUS_UPDATER_BOOT_SIZE};HUBUS_UPD_APP_SIZE=${app_size}"
        "-nostartfiles;-mrelax")

    # An application with update support, linked behind the boot section: the reference firmware with
    # HUBUS_ENABLE_UPDATE. It has to leave the info page, the last page of the area, free.
    add_executable(hubus_size_reference_update ${PROJECT_SOURCE_DIR}/slave/size_reference.c
        ${PROJECT_SOURCE_DIR}/slave/hubus_slave.c ${PROJECT_SOURCE_DIR}/core/hubus_slave_core.c
        ${PROJECT_SOURCE_DIR}/core/hubus_crc.c ${PROJECT_SOURCE_DIR}/core/hubus_frame.c)
    target_include_directories(hubus_size_reference_update PRIVATE ${PROJECT_SOURCE_DIR}/core ${PROJECT_SOURCE_DIR}/slave)
    target_compile_definitions(hubus_size_reference_update PRIVATE
        HUBUS_CLK_PORT=${clk_port} HUBUS_CLK_PIN=${clk_pin} HUBUS_DATA_PORT=${data_port} HUBUS_DATA_PIN=${data_pin}
        HUBUS_TX_BUF_SIZE=${HUBUS_TX_BUF_SIZE} HUBUS_RX_BUF_SIZE=${HUBUS_RX_BUF_SIZE}
        HUBUS_ENABLE_EVENTS=1 HUBUS_ENABLE_STATE=1 HUBUS_ENABLE_UPDATE=1)
    target_compile_options(hubus_size_reference_update PRIVATE ${HUBUS_WARNINGS})
    target_link_options(hubus_size_reference_update PRIVATE -Wl,--section-start=.text=${app_start_hex})
    set_target_properties(hubus_size_reference_update PROPERTIES SUFFIX ".elf")
    math(EXPR app_space "${app_size} - 128") # the info page: 64 or 128 bytes
    add_custom_target(hubus_size_reference_update_size ALL
        COMMAND ${AVR_OBJCOPY} -O ihex -R .eeprom $<TARGET_FILE:hubus_size_reference_update>
                ${CMAKE_CURRENT_BINARY_DIR}/hubus_size_reference_update.hex
        COMMAND ${CMAKE_COMMAND} -DSIZE_TOOL=${AVR_SIZE} -DELF=$<TARGET_FILE:hubus_size_reference_update>
                -DMCU=${HUBUS_AVR_MCU} -DMAX_FLASH=${app_space} -DMAX_RAM=100000 -DENFORCE=ON
                -P ${PROJECT_SOURCE_DIR}/cmake/check-size.cmake
        DEPENDS hubus_size_reference_update
        COMMENT "Checking that an application with update support fits behind the boot section")
endif()
