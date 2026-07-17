#!/usr/bin/env bash
# Source this for ESP-IDF builds. Lives on the root disk (survives reboot).
export IDF_PATH="${IDF_PATH:-/root/esp/esp-idf}"
export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-/root/esp/idf-tools}"
# shellcheck disable=SC1091
source "$IDF_PATH/export.sh"
