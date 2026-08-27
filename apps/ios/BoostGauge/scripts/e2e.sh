#!/usr/bin/env bash
# E2E: boot a simulator, install and launch BoostGauge against the repo mock
# server, wait, screenshot, and confirm the app polled /state.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
APP_ROOT="$REPO_ROOT/apps/ios/BoostGauge"
PORT="${1:-18099}"
MOCK_URL="http://127.0.0.1:$PORT"
if [ -z "${SIM_NAME:-}" ]; then
  SIM_NAME="$(xcrun simctl list devices available 2>/dev/null | grep -oE "iPhone 1[67]( Pro)?\b" | head -1)"
  SIM_NAME="${SIM_NAME:-iPhone}"
fi
DERIVED="$APP_ROOT/DerivedData"
SCREENSHOT="${SCREENSHOT:-/tmp/ios_screen.png}"
MOCK_LOG=/tmp/boost-gauge-mock.log

if ! xcodebuild -version >/dev/null 2>&1; then
  echo "e2e: Xcode not available yet; run scripts/check_xcode.sh first" >&2
  exit 2
fi

echo "e2e: starting mock server on port $PORT"
pkill -f "mock_server.py --host 127.0.0.1 --port $PORT" 2>/dev/null || true
python3 -u "$REPO_ROOT/tools/mock_server.py" --host 127.0.0.1 --port "$PORT" --verbose >"$MOCK_LOG" 2>&1 &
MOCK_PID=$!
trap 'kill "$MOCK_PID" 2>/dev/null || true' EXIT
sleep 1
STATE_BODY="$(curl -fsS "$MOCK_URL/api/v1/state" | head -c 80)" || {
  echo "e2e: mock server not responding" >&2
  exit 1
}
echo "e2e: mock /state -> $STATE_BODY"

echo "e2e: building for simulator"
xcodebuild build \
  -project "$APP_ROOT/BoostGauge.xcodeproj" \
  -scheme BoostGauge \
  -configuration Debug \
  -destination "platform=iOS Simulator,name=$SIM_NAME" \
  -derivedDataPath "$DERIVED" \
  CODE_SIGNING_ALLOWED=NO |
  tail -5

APP_BUNDLE="$DERIVED/Build/Products/Debug-iphonesimulator/BoostGauge.app"
if [ ! -d "$APP_BUNDLE" ]; then
  echo "e2e: app bundle missing at $APP_BUNDLE" >&2
  exit 1
fi

echo "e2e: preparing simulator $SIM_NAME"
if ! xcrun simctl list devices booted | grep -q "$SIM_NAME"; then
  xcrun simctl boot "$SIM_NAME" || true
fi
xcrun simctl bootstatus "$SIM_NAME" -b

echo "e2e: installing and launching"
xcrun simctl install booted "$APP_BUNDLE"
xcrun simctl launch booted com.boostgauge.app -e2eHTTPURL "$MOCK_URL"
sleep 8

xcrun simctl io booted screenshot "$SCREENSHOT"
echo "e2e: screenshot -> $SCREENSHOT"

if grep -q "GET /api/v1/state" "$MOCK_LOG"; then
  echo "e2e: PASS - app polled /api/v1/state"
  grep -c "GET /api/v1/state" "$MOCK_LOG" | xargs echo "e2e: state polls:"
else
  echo "e2e: WARN - no /api/v1/state requests seen in mock log" >&2
  tail -20 "$MOCK_LOG" >&2
  exit 1
fi
