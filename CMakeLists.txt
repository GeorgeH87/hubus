cmake_minimum_required(VERSION 3.21)
project(hubus VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(HUBUS_WERROR "Treat compiler warnings as errors" OFF)
option(HUBUS_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer (host)" OFF)

if(CMAKE_SYSTEM_PROCESSOR STREQUAL "avr")
    set(HUBUS_AVR TRUE)
else()
    set(HUBUS_AVR FALSE)
endif()

set(HUBUS_WARNINGS -Wall -Wextra -Wshadow -Wconversion -Wstrict-prototypes -Wmissing-prototypes)
if(HUBUS_WERROR)
    list(APPEND HUBUS_WARNINGS -Werror)
endif()

# ---------------------------------------------------------------------------
# core/: hardware-independent protocol code shared by master, slave, and tests
# ---------------------------------------------------------------------------
set(HUBUS_CORE_SOURCES
    core/hubus_crc.c
    core/hubus_frame.c
    core/hubus_arb.c
    core/hubus_gen.c
    core/hubus_slave_core.c)

set(HUBUS_CORE_SOURCES_ABS ${HUBUS_CORE_SOURCES})
list(TRANSFORM HUBUS_CORE_SOURCES_ABS PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")

add_library(hubus_core STATIC ${HUBUS_CORE_SOURCES})
target_include_directories(hubus_core PUBLIC core)
target_compile_options(hubus_core PRIVATE ${HUBUS_WARNINGS})

if(HUBUS_AVR)
    add_subdirectory(slave)
    return()
endif()

if(HUBUS_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# master/: protocol master on a HAL (libgpiod HAL for the Raspberry Pi)
# ---------------------------------------------------------------------------
add_library(hubus_master STATIC master/hubus_master.c)
target_include_directories(hubus_master PUBLIC master)
target_link_libraries(hubus_master PUBLIC hubus_core)
target_compile_options(hubus_master PRIVATE ${HUBUS_WARNINGS})

add_subdirectory(master)
add_subdirectory(daemon)

# ---------------------------------------------------------------------------
# slave/ on the host (host HAL): lets tests exercise the public slave API
# ---------------------------------------------------------------------------
add_subdirectory(slave)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(test)
endif()
