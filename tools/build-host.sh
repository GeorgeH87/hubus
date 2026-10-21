#!/bin/sh
# Configure, build, and test the host targets (tests, simulation, master).
# Usage (inside WSL or on Linux): sh tools/build-host.sh [ctest args]
set -eu
cd "$(dirname "$0")/.."
cmake --preset host
cmake --build --preset host
ctest --preset host "$@"
