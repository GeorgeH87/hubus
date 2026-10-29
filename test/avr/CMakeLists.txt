# Slave firmware on simulated AVR devices (simavr): the AVR build of the slave library against the real master on the
# bus simulation. Skipped if simavr (libsimavr-dev) or avr-gcc is not installed.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SIMAVR QUIET IMPORTED_TARGET simavr)
endif()
find_program(HUBUS_AVR_GCC avr-gcc)
if(NOT SIMAVR_FOUND OR NOT HUBUS_AVR_GCC)
    message(STATUS "simavr or avr-gcc not found: AVR simulation tests skipped")
    return()
endif()

# The test firmware, built with the AVR toolchain on every build.
include(ExternalProject)
set(HUBUS_AVR_FW_DIR ${CMAKE_CURRENT_BINARY_DIR}/firmware)
ExternalProject_Add(hubus_avr_test_firmware
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/firmware
    BINARY_DIR ${HUBUS_AVR_FW_DIR}
    CMAKE_ARGS
        -DCMAKE_TOOLCHAIN_FILE=${PROJECT_SOURCE_DIR}/cmake/avr-gcc.cmake
        -DCMAKE_BUILD_TYPE=MinSizeRel
        -DHUBUS_AVR_MCU=attiny85
        -DHUBUS_ROOT=${PROJECT_SOURCE_DIR}
        -DHUBUS_WERROR=${HUBUS_WERROR}
    INSTALL_COMMAND ""
    BUILD_ALWAYS TRUE)

add_library(hubus_avr_sim STATIC avr_slave.c)
target_include_directories(hubus_avr_sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hubus_avr_sim PUBLIC hubus_sim unity PkgConfig::SIMAVR)
target_compile_options(hubus_avr_sim PRIVATE ${HUBUS_TEST_WARNINGS})

add_executable(test_avr_slave test_avr_slave.c)
target_link_libraries(test_avr_slave PRIVATE hubus_avr_sim)
target_compile_definitions(test_avr_slave PRIVATE HUBUS_AVR_FW_DIR="${HUBUS_AVR_FW_DIR}")
target_compile_options(test_avr_slave PRIVATE ${HUBUS_TEST_WARNINGS})
add_dependencies(test_avr_slave hubus_avr_test_firmware)
add_test(NAME test_avr_slave COMMAND test_avr_slave)
set_tests_properties(test_avr_slave PROPERTIES TIMEOUT 300 LABELS avr
    ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp:print_suppressions=0")
