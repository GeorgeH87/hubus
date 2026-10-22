#!/bin/sh
# Installs the HUBUS development toolchain on Ubuntu 26.04 (WSL2 or native).
# Usage: sudo sh tools/setup-ubuntu.sh
set -eu

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y \
    build-essential cmake ninja-build pkg-config git gdb clang-format \
    gcc-avr binutils-avr avr-libc avrdude simavr \
    libgpiod-dev gpiod libyaml-dev libmosquitto-dev mosquitto mosquitto-clients

# Documentation tools (tools/setup-docs.sh installs PlantUML and mermaid-cli on top): Java for PlantUML, Node.js for
# mermaid-cli, Python for the link check, unzip for the browser download, and the libraries of the headless browser that mermaid-cli uses.
apt-get install -y \
    default-jre-headless nodejs npm python3 curl unzip ca-certificates fonts-dejavu-core \
    libasound2t64 libatk-bridge2.0-0t64 libatk1.0-0t64 libatspi2.0-0t64 libdbus-1-3 libdrm2 libgbm1 \
    libglib2.0-0t64 libnspr4 libnss3 libx11-6 libxcomposite1 libxdamage1 libxext6 libxfixes3 libxi6 \
    libxkbcommon0 libxrandr2 libxrender1 libxres1
