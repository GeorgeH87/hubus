#!/bin/sh
# Runs one host test binary with the bus trace enabled.
# Usage: sh tools/run-trace.sh test_master [grep-pattern] [context-lines]
set -eu
cd "$(dirname "$0")/.."
cmake --build --preset host >/dev/null
HUBUS_SIM_TRACE=1 "./build/host/test/$1" 2>&1 | grep -B"${3:-8}" -m3 "${2:-TIMING}" || true
