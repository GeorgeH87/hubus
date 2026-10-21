# Toolchain file for the AVR slave firmware.
#
#   cmake --preset avr                        # ATtiny85 (default)
#   cmake --preset avr -DHUBUS_AVR_MCU=attiny841
#
# HUBUS_AVR_MCU is the -mmcu= device name, HUBUS_F_CPU the clock in Hz.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR avr)

find_program(AVR_GCC avr-gcc REQUIRED)
find_program(AVR_SIZE avr-size REQUIRED)
find_program(AVR_OBJCOPY avr-objcopy REQUIRED)
find_program(AVR_AR avr-gcc-ar)

set(CMAKE_C_COMPILER ${AVR_GCC})
if(AVR_AR)
    set(CMAKE_AR ${AVR_AR})
endif()
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(NOT DEFINED HUBUS_AVR_MCU)
    set(HUBUS_AVR_MCU attiny85 CACHE STRING "AVR device (-mmcu=)")
endif()
if(NOT DEFINED HUBUS_F_CPU)
    set(HUBUS_F_CPU 8000000 CACHE STRING "CPU clock in Hz")
endif()

set(CMAKE_C_FLAGS_INIT "-mmcu=${HUBUS_AVR_MCU} -DF_CPU=${HUBUS_F_CPU}UL -ffunction-sections -fdata-sections -fshort-enums")
set(CMAKE_C_FLAGS_RELEASE_INIT "-Os")
set(CMAKE_C_FLAGS_MINSIZEREL_INIT "-Os")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-mmcu=${HUBUS_AVR_MCU} -Wl,--gc-sections")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
