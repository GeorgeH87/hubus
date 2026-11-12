# Prints the size of an AVR ELF and fails if it exceeds the budget.
# Usage: cmake -DSIZE_TOOL=avr-size -DELF=app.elf -DMCU=attiny85 -DMAX_FLASH=n -DMAX_RAM=n [-DWARN_ONLY=ON]
#        [-DENFORCE=ON] -P check-size.cmake
#
# The budget applies to the ATtiny85 reference build, and with ENFORCE to any
# device (a limit of the layout, such as the boot section); other devices are
# reported but not limited. With WARN_ONLY an exceeded budget is a warning.

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

if(MCU STREQUAL "attiny85" OR ENFORCE)
    if(WARN_ONLY)
        set(level WARNING)
    else()
        set(level FATAL_ERROR)
    endif()
    set(ok TRUE)
    if(flash GREATER MAX_FLASH)
        message(${level} "${MCU} flash budget exceeded: ${flash} > ${MAX_FLASH} bytes")
        set(ok FALSE)
    endif()
    if(ram GREATER MAX_RAM)
        message(${level} "${MCU} RAM budget exceeded: ${ram} > ${MAX_RAM} bytes")
        set(ok FALSE)
    endif()
    if(ok)
        message(STATUS "${MCU} budget ok (flash <= ${MAX_FLASH}, RAM <= ${MAX_RAM})")
    endif()
endif()
