#!/bin/sh
# Installs the HUBUS development toolchain on Ubuntu 26.04 (WSL2 or native).
# Usage: sudo sh tools/setup-ubuntu.sh
set -eu

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y \
    build-essential cmake ninja-build pkg-config git gdb clang-format \
    gcc-avr binutils-avr avr-libc avrdude simavr \
    libgpiod-dev gpiod
