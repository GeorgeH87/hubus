#!/bin/sh
# Builds the slave firmware for one AVR device and checks the flash/RAM budget.
# Usage: sh tools/build-avr.sh [mcu] [extra cmake args]   (default mcu: attiny85)
set -eu
cd "$(dirname "$0")/.."
mcu="${1:-attiny85}"
[ $# -gt 0 ] && shift
cmake --preset avr -B "build/avr-$mcu" -DHUBUS_AVR_MCU="$mcu" "$@"
cmake --build "build/avr-$mcu"
