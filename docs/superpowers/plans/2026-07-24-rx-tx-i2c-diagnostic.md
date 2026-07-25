# RX/TX I2C Diagnostic Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** OTA-flash a temporary firmware image that moves the external sensor bus from GPIO17/GPIO18 to the exposed RX/TX pads so the original GPIO pair can be isolated.

**Architecture:** Keep ESP-IDF I2C port 0, the existing sensor drivers, and the 100 kHz transaction rate. Change only the pin constants to SDA=GPIO44/RX and SCL=GPIO43/TX; GPIO43 is intentionally SCL so early UART0 TX activity cannot create I2C START conditions while SDA remains pulled high.

**Tech Stack:** ESP-IDF 5.5.1, PowerShell, curl, ESP32-S3 OTA.

## Global Constraints

- Preserve all existing uncommitted changes, including the 100 kHz sensor rate.
- Do not alter display, web, sensor-conversion, release artifacts, or generated files.
- Keep any USB-UART adapter disconnected from TX/RX during the hardware test.
- Use `build/boost_gauge.bin` for OTA, never a merged full-flash image.
- This is a temporary diagnostic mapping; leave the source change uncommitted so it can be reverted independently after the test.

---

### Task 1: Prove the Current Source Does Not Have the Diagnostic Mapping

**Files:**
- Inspect: `main/boost_sensors.c`

**Interfaces:**
- Consumes: `SENS_SCL_GPIO` and `SENS_SDA_GPIO` preprocessor definitions.
- Produces: A failing pre-change assertion that becomes the post-change guard.

- [ ] **Step 1: Run the diagnostic mapping assertion**

```powershell
$source = Get-Content -LiteralPath 'main\boost_sensors.c' -Raw
if ($source -notmatch '#define\s+SENS_SCL_GPIO\s+43\b' -or
    $source -notmatch '#define\s+SENS_SDA_GPIO\s+44\b') {
    throw 'RX/TX diagnostic mapping is absent'
}
```

Expected: FAIL with `RX/TX diagnostic mapping is absent`, because the current
source uses SCL=18 and SDA=17.

### Task 2: Apply the Minimal Pin Remap

**Files:**
- Modify: `main/boost_sensors.c:27-28`

**Interfaces:**
- Consumes: Exposed rear-header TX=GPIO43 and RX=GPIO44.
- Produces: I2C port 0 configured as SCL=GPIO43 and SDA=GPIO44.

- [ ] **Step 1: Change only the two pin constants**

```c
#define SENS_SCL_GPIO       43  /* exposed TX pad; boot UART chatter is clocks */
#define SENS_SDA_GPIO       44  /* exposed RX pad */
```

- [ ] **Step 2: Re-run the diagnostic mapping assertion**

Run the PowerShell assertion from Task 1.

Expected: PASS with exit code 0.

- [ ] **Step 3: Check the focused diff**

```powershell
git diff --check -- main/boost_sensors.c
git diff -- main/boost_sensors.c
```

Expected: the existing 400-to-100 kHz change plus the intended GPIO18/17-to-
GPIO43/44 diagnostic mapping; no unrelated source changes.

### Task 3: Build the ESP-IDF Application

**Files:**
- Produce: `build/boost_gauge.bin`

**Interfaces:**
- Consumes: ESP-IDF 5.5.1 installation at `C:\esp\v5.5.1\esp-idf`.
- Produces: Valid ESP32-S3 OTA application image.

- [ ] **Step 1: Export the installed toolchain and build**

```powershell
$env:IDF_TOOLS_PATH = 'C:\Espressif'
& 'C:\esp\v5.5.1\esp-idf\export.ps1'
Set-Location 'C:\Users\aliab\boost-gauge'
idf.py build
```

Expected: exit code 0 and `build/boost_gauge.bin` produced.

- [ ] **Step 2: Verify the artifact and source mapping**

```powershell
Get-Item -LiteralPath 'build\boost_gauge.bin' | Select-Object FullName,Length,LastWriteTimeUtc
Get-FileHash -Algorithm SHA256 -LiteralPath 'build\boost_gauge.bin'
$source = Get-Content -LiteralPath 'main\boost_sensors.c' -Raw
if ($source -notmatch '#define\s+SENS_SCL_GPIO\s+43\b' -or
    $source -notmatch '#define\s+SENS_SDA_GPIO\s+44\b' -or
    $source -notmatch '#define\s+SENS_I2C_HZ\s+100000\b') {
    throw 'Built source configuration is not the approved diagnostic mapping'
}
```

Expected: non-empty image, fresh timestamp, SHA-256 printed, and assertion pass.

### Task 4: OTA-Flash and Verify Control-Plane Recovery

**Files:**
- Read: `build/boost_gauge.bin`

**Interfaces:**
- Consumes: Gauge API at `http://192.168.50.102/api/v1`.
- Produces: Validated OTA image selected, restarted gauge, reachable HTTP state.

- [ ] **Step 1: Record the pre-OTA state**

```powershell
curl.exe --fail-with-body --show-error --silent --max-time 5 `
  http://192.168.50.102/api/v1/state
```

Expected: JSON state with the old uptime.

- [ ] **Step 2: Upload the application image**

```powershell
curl.exe --fail-with-body --show-error --silent --max-time 120 `
  -H 'Content-Type: application/octet-stream' `
  --data-binary '@C:\Users\aliab\boost-gauge\build\boost_gauge.bin' `
  http://192.168.50.102/api/v1/ota
```

Expected: JSON containing `"ok":true`, the exact image byte count, and
`"restartRequired":true`.

- [ ] **Step 3: Restart the gauge**

```powershell
curl.exe --fail-with-body --show-error --silent --max-time 5 -X POST `
  http://192.168.50.102/api/v1/restart
```

Expected: `{"ok":true,"restartingInMs":400}` or a dropped response after the
restart begins.

- [ ] **Step 4: Poll for the returned control plane**

```powershell
$deadline = (Get-Date).AddSeconds(120)
do {
    Start-Sleep -Seconds 2
    $state = curl.exe --fail-with-body --show-error --silent --max-time 3 `
      http://192.168.50.102/api/v1/state 2>$null
} until ($LASTEXITCODE -eq 0 -or (Get-Date) -ge $deadline)
if ($LASTEXITCODE -ne 0) { throw 'Gauge did not return after OTA restart' }
$state
```

Expected: JSON state with a reset uptime. Remote-only evidence proves upload,
restart, and HTTP recovery; exact OTA partition identity still requires serial
boot logs.

### Task 5: Rewire and Run the Hardware Diagnostic

**Files:**
- None.

**Interfaces:**
- Consumes: BMP280 direct 3.3 V wiring and two 4.7 kOhm pull-ups.
- Produces: Stable expected device address and ambient-pressure reading, or a
  falsification of the original-GPIO-only damage hypothesis.

- [ ] **Step 1: Rewire with all power removed**

Move BMP280 SCL and its pull-up to TX/GPIO43. Move BMP280 SDA and its pull-up to
RX/GPIO44. Keep VDD=3.3 V, common ground, SDO=GND, and CS/CSB=3.3 V when present.

- [ ] **Step 2: Power up and run five scans**

```powershell
1..5 | ForEach-Object {
    curl.exe --fail-with-body --show-error --silent --max-time 5 `
      http://192.168.50.102/api/v1/sensors/scan
    Start-Sleep -Milliseconds 500
}
```

Expected for a successful alternate-pin test: every scan includes only stable,
real addresses, including BMP280 `0x76`.

- [ ] **Step 3: Verify live BMP state**

```powershell
curl.exe --fail-with-body --show-error --silent --max-time 5 `
  http://192.168.50.102/api/v1/state
```

Expected: `"bmpPresent":true` and a plausible non-default `ambientKpa`.

