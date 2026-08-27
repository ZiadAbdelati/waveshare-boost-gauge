#!/usr/bin/env bash
# Simulator verification for the Settings connection indicator and freeze
# soak. Requires a built app in DerivedData2 (xcodebuild test/build first).
#
#   scripts/sim_settings_verify.sh            # unreachable-host soak, then live mock
#   scripts/sim_settings_verify.sh --dead     # unreachable-host soak only
#
# Artifacts: /tmp/ios_settings_unreachable.png, /tmp/ios_settings_connected.png
set -euo pipefail

APP_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "$APP_ROOT/../../.." && pwd)"
SIM_NAME="${SIM_NAME:-iPhone 17}"
DERIVED="$APP_ROOT/DerivedData2"
APP_BUNDLE="$DERIVED/Build/Products/Debug-iphonesimulator/BoostGauge.app"
MOCK_PORT="${MOCK_PORT:-18099}"
MOCK_URL="http://127.0.0.1:$MOCK_PORT"
MOCK_LOG=/tmp/boost-gauge-mock-settings.log
DEAD_URL="http://127.0.0.1:9"
MOCK_PID=""

log() { echo "[sim-verify] $*"; }

ensure_device() {
  if ! xcrun simctl list devices booted | grep -q "$SIM_NAME"; then
    log "booting $SIM_NAME"
    xcrun simctl boot "$SIM_NAME" || true
  fi
  xcrun simctl bootstatus "$SIM_NAME" -b >/dev/null
}

host_pid() {
  pgrep -x BoostGauge || pgrep -f "BoostGauge.app/BoostGauge" || true
}

soak_check() {
  local before="$1"
  local after="$2"
  local label="$3"
  log "$label: screenshot 1 -> $before"
  sleep 30
  xcrun simctl io booted screenshot "$after" >/dev/null
  log "$label: screenshot 2 (after 30 s) -> $after"

  local pid
  pid="$(host_pid)"
  if [ -z "$pid" ]; then
    log "$label: FAIL - BoostGauge host process not found"
    return 1
  fi
  log "$label: app host process alive (pid $pid)"

  local device_alive
  device_alive="$(xcrun simctl spawn booted launchctl list | grep -i boostgauge || true)"
  if [ -z "$device_alive" ]; then
    log "$label: FAIL - launchctl does not list bootstrap boostgauge"
    return 1
  fi
  log "$label: launchctl entry: $device_alive"

  if ! cmp -s "$before" "$after"; then
    log "$label: PASS - screenshots differ; UI updated during the soak"
    return 0
  fi

  local cpu1 cpu2
  cpu1="$(ps -o %cpu= -p "$pid" | tr -d ' ')"
  sleep 2
  cpu2="$(ps -o %cpu= -p "$pid" | tr -d ' ')"
  log "$label: screenshots identical; host CPU samples $cpu1% / $cpu2%"
  awk -v c1="$cpu1" -v c2="$cpu2" 'BEGIN { if (c1 < 40 && c2 < 40) exit 0; exit 1 }' || {
    log "$label: FAIL - UI static AND process busy (possible runaway re-render)"
    return 1
  }
  log "$label: PASS - UI static by design (idle screen) and process not busy"
  return 0
}

run_dead_host() {
  log "== dead-host run: $DEAD_URL =="
  xcrun simctl terminate booted com.boostgauge.app 2>/dev/null || true
  xcrun simctl install booted "$APP_BUNDLE"
  xcrun simctl launch booted com.boostgauge.app -e2eHTTPURL "$DEAD_URL" -e2eTab settings >/dev/null
  sleep 4
  xcrun simctl io booted screenshot /tmp/ios_settings_unreachable.png >/dev/null
  soak_check /tmp/ios_settings_unreachable.png /tmp/ios_settings_unreachable_after30.png "dead-host"
}

run_live_mock() {
  log "== live mock run: $MOCK_URL =="
  pkill -f "mock_server.py --host 127.0.0.1 --port $MOCK_PORT" 2>/dev/null || true
  python3 -u "$REPO_ROOT/tools/mock_server.py" --host 127.0.0.1 --port "$MOCK_PORT" --verbose >"$MOCK_LOG" 2>&1 &
  MOCK_PID=$!
  trap 'kill "$MOCK_PID" 2>/dev/null || true' EXIT
  sleep 1
  curl -fsS "$MOCK_URL/api/v1/state" >/dev/null || {
    log "FAIL - mock server not serving"
    return 1
  }

  xcrun simctl terminate booted com.boostgauge.app 2>/dev/null || true
  xcrun simctl install booted "$APP_BUNDLE"
  xcrun simctl launch booted com.boostgauge.app -e2eHTTPURL "$MOCK_URL" -e2eTab settings >/dev/null
  sleep 4
  xcrun simctl io booted screenshot /tmp/ios_settings_connected.png >/dev/null
  log "screenshot -> /tmp/ios_settings_connected.png"
  if grep -q "GET /api/v1/state" "$MOCK_LOG"; then
    log "PASS - mock saw /api/v1/state: $(grep -c 'GET /api/v1/state' "$MOCK_LOG") requests"
  else
    log "WARN - no /api/v1/state requests observed"
    tail -20 "$MOCK_LOG" >&2
  fi
}

if [ ! -d "$APP_BUNDLE" ]; then
  log "missing app bundle at $APP_BUNDLE; build first"
  exit 2
fi

ensure_device
run_dead_host
if [ "${1:-}" != "--dead" ]; then
  run_live_mock
fi
log "done; screenshots in /tmp/ios_settings_*.png"
