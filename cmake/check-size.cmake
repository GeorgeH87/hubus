# Prints the size of an AVR ELF and fails if it exceeds the budget.
# Usage: cmake -DSIZE_TOOL=avr-size -DELF=app.elf -DMCU=attiny85 -DMAX_FLASH=n -DMAX_RAM=n -P check-size.cmake
#
# The budget only applies to the ATtiny85 reference build; other devices are
# reported but not limited.

execute_process(COMMAND ${SIZE_TOOL} --format=berkeley ${ELF}
    OUTPUT_VARIABLE out RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "avr-size failed for ${ELF}")
endif()

string(REPLACE "\n" ";" lines "${out}")
list(GET lines 1 values)
string(REGEX MATCHALL "[0-9]+" nums "${values}")
list(GET nums 0 text)
list(GET nums 1 data)
list(GET nums 2 bss)
math(EXPR flash "${text} + ${data}")
math(EXPR ram "${data} + ${bss}")

message(STATUS "${MCU}: flash ${flash} bytes, RAM ${ram} bytes (data ${data} + bss ${bss})")

if(MCU STREQUAL "attiny85")
    if(flash GREATER MAX_FLASH)
        message(FATAL_ERROR "ATtiny85 flash budget exceeded: ${flash} > ${MAX_FLASH} bytes")
    endif()
    if(ram GREATER MAX_RAM)
        message(FATAL_ERROR "ATtiny85 RAM budget exceeded: ${ram} > ${MAX_RAM} bytes")
    endif()
    message(STATUS "ATtiny85 budget ok (flash <= ${MAX_FLASH}, RAM <= ${MAX_RAM})")
endif()
