#!/usr/bin/env bash
# Flash prebuilt boost_gauge firmware (OTA partition layout)
# Usage: ./flash.sh /dev/ttyACM0
set -euo pipefail
PORT="${1:-}"
if [[ -z "$PORT" ]]; then
  echo "Usage: $0 <serial-port>"
  exit 1
fi
DIR="$(cd "$(dirname "$0")" && pwd)"
python -m esptool --chip esp32s3 -p "$PORT" -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x0 "$DIR/boost_gauge_merged.bin"
