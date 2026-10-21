#!/bin/sh
# Runs only some mutants: sh tools/run-mutants.sh "wrong discovery" "duplicate MCID"
cd "$(dirname "$0")/.."
for pattern in "$@"; do
    ONLY="$pattern" sh tools/mutation-check.sh | head -1
done
