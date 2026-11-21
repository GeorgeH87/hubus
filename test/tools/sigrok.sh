#!/bin/sh
# Test of the sigrok decoder (tools/sigrok/hubus): records the bus scenarios of sigrok_scenarios as VCD files and
# decodes them with sigrok-cli. The lines of test/sigrok/<scenario>.txt have to appear in this order, each as part of
# a line of the output; every warning of the decoder has to be one of those lines.
# Usage: sigrok.sh <sigrok-cli> <sigrok_scenarios> <source dir> <work dir>
set -eu

cli=$1
scenarios=$2
src=$3
work=$4

mkdir -p "$work"
rm -f "$work"/*.vcd
if ! "$scenarios" "$work" >"$work/scenarios.log" 2>&1; then
    cat "$work/scenarios.log"
    exit 1
fi
SIGROKDECODE_DIR="$src/tools/sigrok"
export SIGROKDECODE_DIR

decode() { # vcd, rows
    "$cli" -I vcd -i "$1" -P hubus:clk=clk:data=data -A "hubus=$2" 2>&1 | sed 's/^hubus-1: //'
}

fail=0
for vcd in "$work"/*.vcd; do
    name=$(basename "$vcd" .vcd)
    expect="$src/test/sigrok/$name.txt"
    out="$work/$name.out"
    if [ ! -f "$expect" ]; then
        echo "$name: no expectation in test/sigrok/$name.txt"
        fail=1
        continue
    fi
    decode "$vcd" conditions:frames:warnings:slots >"$out"
    if ! awk '
        NR == FNR { if ($0 != "" && substr($0, 1, 1) != "#") want[++n] = $0; next }
        i < n && index($0, want[i + 1]) { i++ }
        END { if (i < n) { print "missing, in this order: " want[i + 1]; exit 1 } }' "$expect" "$out"; then
        echo "$name: unexpected decoder output, see $out"
        fail=1
    fi
    decode "$vcd" warnings >"$out.warnings"
    while IFS= read -r w; do
        if ! grep -Fxq -- "$w" "$expect"; then
            echo "$name: unexpected warning: $w"
            fail=1
        fi
    done <"$out.warnings"
done
[ "$fail" -eq 0 ] && echo "sigrok decoder ok"
exit "$fail"
