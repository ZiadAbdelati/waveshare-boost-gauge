#!/usr/bin/env bash
# Waits for Xcode 26.x to be installed at /Applications, selects it, runs
# first-launch setup, and lists available simulators.
set -u

log() { echo "[check-xcode] $*"; }

if ! xcodebuild -version >/dev/null 2>&1; then
  for i in $(seq 1 240); do
    sleep 30
    if xcodebuild -version >/dev/null 2>&1; then
      break
    fi
    log "still waiting for Xcode (elapsed $((i * 30))s)"
  done
fi

if ! xcodebuild -version >/dev/null 2>&1; then
  log "Xcode did not appear within the wait window"
  exit 1
fi

log "found:"
xcodebuild -version

EXPECTED="/Applications/Xcode.app/Contents/Developer"
CURRENT="$(xcode-select -p 2>/dev/null || true)"
if [ "$CURRENT" != "$EXPECTED" ]; then
  log "switching developer directory to $EXPECTED"
  if ! xcode-select -s "$EXPECTED" 2>/dev/null; then
    sudo -n xcode-select -s "$EXPECTED" 2>/dev/null ||
      log "could not switch developer dir (interactive sudo required)"
  fi
fi

log "running first-launch setup"
xcodebuild -runFirstLaunch >/tmp/xcode-first-launch.log 2>&1 || true
tail -3 /tmp/xcode-first-launch.log

log "available simulators:"
xcrun simctl list devices available | head -40
