if(HUBUS_AVR)
    include(${PROJECT_SOURCE_DIR}/cmake/avr-devices.cmake)

    set(HUBUS_CLK_PORT "" CACHE STRING "Port letter of CLK (empty: device default)")
    set(HUBUS_CLK_PIN "" CACHE STRING "Bit number of CLK")
    set(HUBUS_DATA_PORT "" CACHE STRING "Port letter of DATA")
    set(HUBUS_DATA_PIN "" CACHE STRING "Bit number of DATA")
    # Optional parts, chosen per device type (core/hubus_slave_core.h); every combination is valid.
    set(HUBUS_ENABLE_EVENTS "1" CACHE STRING "Event buffer: announces events, REQUEST_DATA, STATUS_CAPTURE (1: yes, 0: no)")
    set(HUBUS_ENABLE_STATE "1" CACHE STRING "Live state for REQUEST_STATE (1: yes, 0: no)")
    # Devices with 128 B RAM: two 31-byte buffers would leave too little RAM for the stack (the ISR path needs about
    # 40-50 bytes), so they default to 20-byte buffers. Without the event buffer only the command buffer remains,
    # and 31 bytes fit.
    if(HUBUS_ENABLE_EVENTS AND HUBUS_AVR_MCU MATCHES "^attiny(24a?|25|261a?|2313a?|202|204|212|214)$")
        set(HUBUS_BUF_SIZE_DEFAULT 20)
    else()
        set(HUBUS_BUF_SIZE_DEFAULT 31)
    endif()
    set(HUBUS_TX_BUF_SIZE "${HUBUS_BUF_SIZE_DEFAULT}" CACHE STRING "Outgoing buffer size in bytes (1..31)")
    set(HUBUS_RX_BUF_SIZE "${HUBUS_BUF_SIZE_DEFAULT}" CACHE STRING "Largest accepted command payload in bytes (1..31)")

    if(HUBUS_CLK_PORT STREQUAL "")
        hubus_avr_default_pins(${HUBUS_AVR_MCU} clk_port clk_pin data_port data_pin)
    else()
        set(clk_port ${HUBUS_CLK_PORT})
        set(clk_pin ${HUBUS_CLK_PIN})
        set(data_port ${HUBUS_DATA_PORT})
        set(data_pin ${HUBUS_DATA_PIN})
    endif()

    add_library(hubus_slave STATIC hubus_slave.c)
    target_include_directories(hubus_slave PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(hubus_slave PUBLIC hubus_core)
    target_compile_definitions(hubus_slave PUBLIC
        HUBUS_CLK_PORT=${clk_port} HUBUS_CLK_PIN=${clk_pin}
        HUBUS_DATA_PORT=${data_port} HUBUS_DATA_PIN=${data_pin})
    target_compile_options(hubus_slave PRIVATE ${HUBUS_WARNINGS})
    # The core is compiled with the same buffer sizes and parts as the library.
    target_compile_definitions(hubus_core PUBLIC
        HUBUS_TX_BUF_SIZE=${HUBUS_TX_BUF_SIZE} HUBUS_RX_BUF_SIZE=${HUBUS_RX_BUF_SIZE}
        HUBUS_ENABLE_EVENTS=${HUBUS_ENABLE_EVENTS} HUBUS_ENABLE_STATE=${HUBUS_ENABLE_STATE})

    # Reference firmware: uses the complete API and is the basis of the flash/RAM budget.
    add_executable(hubus_size_reference size_reference.c)
    target_link_libraries(hubus_size_reference PRIVATE hubus_slave)
    target_compile_options(hubus_size_reference PRIVATE ${HUBUS_WARNINGS})
    set_target_properties(hubus_size_reference PROPERTIES SUFFIX ".elf")

    # Budget of the ATtiny85 reference build (library + reference firmware).
    set(HUBUS_BUDGET_FLASH 2050 CACHE STRING "ATtiny85 flash budget in bytes")
    set(HUBUS_BUDGET_RAM 112 CACHE STRING "ATtiny85 RAM budget in bytes (data + bss)")
    add_custom_target(hubus_size ALL
        COMMAND ${CMAKE_COMMAND} -DSIZE_TOOL=${AVR_SIZE} -DELF=$<TARGET_FILE:hubus_size_reference>
                -DMCU=${HUBUS_AVR_MCU} -DMAX_FLASH=${HUBUS_BUDGET_FLASH} -DMAX_RAM=${HUBUS_BUDGET_RAM}
                -P ${PROJECT_SOURCE_DIR}/cmake/check-size.cmake
        DEPENDS hubus_size_reference
        COMMENT "Checking flash/RAM budget")

    add_subdirectory(${PROJECT_SOURCE_DIR}/updater ${CMAKE_BINARY_DIR}/updater)
    add_subdirectory(${PROJECT_SOURCE_DIR}/examples ${CMAKE_BINARY_DIR}/examples)
else()
    # Host build of the slave library with the host HAL (pins are variables).
    add_library(hubus_slave_host STATIC hubus_slave.c hal/hubus_hal_host.c)
    target_include_directories(hubus_slave_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(hubus_slave_host PUBLIC hubus_core)
    target_compile_options(hubus_slave_host PRIVATE ${HUBUS_WARNINGS})
endif()
