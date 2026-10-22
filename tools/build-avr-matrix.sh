#!/bin/sh
# Builds the size reference firmware for a matrix of AVR devices (also used by CI).
# Usage: sh tools/build-avr-matrix.sh [mcu ...]
set -eu
cd "$(dirname "$0")/.."
if [ $# -eq 0 ]; then
    set -- attiny25 attiny45 attiny85 attiny24 attiny44 attiny84 attiny441 attiny841 attiny48 attiny88 \
        attiny261 attiny461 attiny861 attiny2313 attiny2313a attiny4313 attiny1634 \
        attiny202 attiny402 attiny412 attiny804 attiny814 attiny1614 attiny1616 attiny1624 attiny3216 attiny3217 \
        attiny424 attiny824
fi
for mcu in "$@"; do
    printf '== %s: ' "$mcu"
    rm -rf "build/avr-$mcu"
    cmake --preset avr -B "build/avr-$mcu" -DHUBUS_AVR_MCU="$mcu" >/dev/null
    cmake --build "build/avr-$mcu" 2>&1 | grep -E "error|Error|flash .* RAM" || true
done
