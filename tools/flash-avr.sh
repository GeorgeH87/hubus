#!/bin/sh
# Writes a firmware image and the MCID of the device in one avrdude call: one image for all devices, one EEPROM byte
# per device (docs/slave-library.md#assigning-the-mcid).
#
# Usage: sh tools/flash-avr.sh [options] <image.hex | -> [mcid]
#   -m MCU         device as for avr-gcc -mmcu (default attiny85)
#   -c PROGRAMMER  avrdude programmer (default usbasp; serialupdi for the 0/1/2-series)
#   -P PORT        avrdude port, for serial programmers
#   -n             print the avrdude command and the EEPROM image instead of running avrdude
#
# image.hex  the firmware (Intel HEX): the updater, or an application without update support; "-" writes only the MCID
# mcid       1..63, into the last byte of the EEPROM, where the slave library (hubus_slave_mcid_from_eeprom()) and the
#            updater look for it. Without it the EEPROM stays as it is, which after the chip erase of a flash write
#            means erased unless the fuse EESAVE is programmed: the firmware then runs with its default MCID.
#
# Needs avrdude 7.0 or newer (it writes only the byte the EEPROM image holds) and avr-gcc for the EEPROM size.
set -eu

mcu=attiny85
programmer=usbasp
port=
dry=0
while getopts "m:c:P:n" opt; do
    case "$opt" in
    m) mcu=$OPTARG ;;
    c) programmer=$OPTARG ;;
    P) port=$OPTARG ;;
    n) dry=1 ;;
    *) sed -n '5,10p' "$0" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
if [ $# -lt 1 ] || [ $# -gt 2 ]; then
    sed -n '5,10p' "$0" >&2
    exit 2
fi
image=$1
mcid=${2:-}

if [ "$image" != "-" ] && [ ! -r "$image" ]; then
    echo "flash-avr: cannot read $image" >&2
    exit 1
fi
if [ "$image" = "-" ] && [ -z "$mcid" ]; then
    echo "flash-avr: nothing to write: give an image, an MCID, or both" >&2
    exit 2
fi

set -- -p "$mcu" -c "$programmer"
[ -n "$port" ] && set -- "$@" -P "$port"
[ "$image" != "-" ] && set -- "$@" -U "flash:w:$image:i"

if [ -n "$mcid" ]; then
    case "$mcid" in
    *[!0-9]* | "") echo "flash-avr: MCID '$mcid' is not a number 1..63" >&2; exit 2 ;;
    esac
    if [ "$mcid" -lt 1 ] || [ "$mcid" -gt 63 ]; then
        echo "flash-avr: MCID $mcid out of range 1..63" >&2
        exit 2
    fi
    # Offset of the last EEPROM byte, as in slave/hubus_slave.h: on the 0/1/2-series E2END is a data address.
    expr=$(printf '#ifdef MAPPED_EEPROM_START\nEEPROM_SIZE - 1\n#else\nE2END\n#endif\n' |
        avr-gcc -mmcu="$mcu" -E -P -include avr/io.h - | tr -d 'Uu' | tail -n 1)
    addr=$(($expr))
    if [ "$addr" -le 0 ] || [ "$addr" -gt 65535 ]; then
        echo "flash-avr: no EEPROM size for $mcu" >&2
        exit 1
    fi
    # One Intel HEX data record with the MCID at that offset, then the end record.
    hi=$((addr >> 8))
    lo=$((addr & 255))
    sum=$(((256 - (1 + hi + lo + mcid) % 256) % 256))
    eep=$(mktemp "${TMPDIR:-/tmp}/hubus-mcid-XXXXXX")
    trap 'rm -f "$eep"' EXIT
    printf ':01%02X%02X00%02X%02X\n:00000001FF\n' "$hi" "$lo" "$mcid" "$sum" >"$eep"
    set -- "$@" -U "eeprom:w:$eep:i"
    echo "flash-avr: MCID $mcid into EEPROM byte $addr of the $mcu"
else
    echo "flash-avr: no MCID given: the EEPROM stays as the chip erase leaves it (erased unless EESAVE is programmed)," \
        "so the firmware uses its default MCID" >&2
fi

if [ "$dry" -eq 1 ]; then
    echo "avrdude $*"
    [ -n "$mcid" ] && cat "$eep"
    exit 0
fi
avrdude "$@"
