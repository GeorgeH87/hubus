#!/bin/sh
# Prints the code symbols of the size reference firmware by size.
# Usage: sh tools/sizes.sh [mcu]
set -eu
cd "$(dirname "$0")/.."
elf="build/avr-${1:-attiny85}/slave/hubus_size_reference.elf"
avr-nm --size-sort -S -t d "$elf" | grep -E " [TtWw] " | grep -v -E " (.L|L0|.Loc)" | tail -20
avr-size -A "$elf" | head -5
