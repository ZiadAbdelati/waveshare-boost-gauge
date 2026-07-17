# Web Control Plane Verification

Mock server:

```sh
python3 tools/mock_server.py --host 127.0.0.1 --port 18080
```

Checks run:

```sh
python3 -m py_compile tools/mock_server.py
node --check web/app.js
curl -I -s http://127.0.0.1:18080/
curl -fsS http://127.0.0.1:18080/api/v1/state
curl -fsS http://127.0.0.1:18080/api/v1/config
curl -fsS http://127.0.0.1:18080/api/v1/themes
curl -fsS http://127.0.0.1:18080/api/v1/logs.csv
printf 'GIF89a-test' | curl -fsS -X POST http://127.0.0.1:18080/api/v1/media \
  -H 'Content-Type: image/gif' -H 'X-Filename: test%20upload.gif' --data-binary @-
head -c 5000 /dev/zero | curl -fsS -X POST http://127.0.0.1:18080/api/v1/ota \
  -H 'Content-Type: application/octet-stream' --data-binary @-
google-chrome-stable --headless=new --no-sandbox --disable-gpu --disable-dev-shm-usage \
  --remote-debugging-port=9222 about:blank
# Chrome DevTools Protocol capture waited 2.4s after navigation, then wrote:
# web/verification/desktop.png at 1440x1100
# web/verification/mobile.png at 390x1200, mobile metrics, deviceScaleFactor 2
```

Results:

- Static UI served from `/` with local assets only.
- `/api/v1/state`, `/config`, `/themes`, `/media/status`, `/logs`, `/logs.csv`, `/time`, `/media`, `/ota`, and `/events` mock paths exercised.
- GIF upload/status preserved the uploaded filename, and OTA upload returned mock acceptance with progress 100.
- Browser text fetch saw the live control surface including gauge, status, schedule, themes, logs, GIF, and OTA controls.
- Desktop screenshot after boot overlay: `web/verification/desktop.png`.
- Mobile screenshot after boot overlay: `web/verification/mobile.png`.
- Project-wide firmware/Android tests intentionally skipped per task instructions.
