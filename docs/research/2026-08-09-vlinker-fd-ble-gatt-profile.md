# vLinker (Vgate) BLE OBD2 adapter — GATT profile and ESP32-S3 integration research

Date: 2026-08-09
Status: research only — no hardware in hand; every claim below is cited to a URL.
Scope: answers AGENTS.md's open gate — *"TPMS BLE central remains feature-flagged off
by default (`BOOST_TPMS_BLE_ENABLED=0`) until a verified vLinker FD+ BLE GATT profile
exists."* The verification still needs a bench unit; this document narrows the search
to a concrete UUID table and a bench-test script, and reports what is already verified
by third-party hardware projects.

## Verdict

- The adapter this repo should target is the **Vgate vLinker FD+** (BLE 4.0 + optional
  WiFi SKU, marketed as **ELM327 V2.2**, "compatible with AT and ST command sets").
  It is the adapter the README/AGENTS.md already name, and it is confirmed BLE-capable:
  on iOS it connects over BLE with **no OS-level pairing** (advertised identity
  `vLinker FD-IOS` / `vLinker FD+`), while the Classic-BT side (`vLinker FD` /
  `vLinker FD-Android`) pairs with PIN `1234`.
- **"vLinker FS+" is not a distinct current Vgate SKU.** The official store lineup is
  MS, FS (BT + USB variants), MC/MC+, BM/BM+, FD (WiFi), FD+ (BLE). No official page,
  download-center entry, or retailer listing for an "FS+" was found. Treat any "FS+"
  mention as a reseller mislabel of the FS family.
- **Vgate publishes no GATT UUIDs.** Independent hardware projects that connect to
  vLinker adapters over BLE converge on **service `0x18F0` with write char `0x2AF1` and
  notify char `0x2AF0`** (base UUID `0000xxxx-0000-1000-8000-00805f9b34fb`), with
  `0xFFF0`/`0xFFF1`/`0xFFF2` and a custom service
  `E7810A71-73AE-499D-8C15-FAA9AEF0C3F2` / char `BEF8D6C9-9C21-4C9E-B632-BD58C1009F9F`
  as observed alternatives on the same hardware. It is **not** the Nordic UART service.
- The link is an **unauthenticated serial-UART GATT**; forcing pairing/bonding makes
  the adapter go silent. Use `setSecurityIOCap(NO_INPUT_OUTPUT)` + auth off, write
  **without response**, subscribe to **notifications**, and pace requests on the ELM
  `>` prompt (plus a ~20 ms vLinker-specific settling delay).

## 1. The vLinker family and which model matters

"vLinker" is the OBD brand of **Vgate Technology Co., Ltd.** (Shenzhen); the brand is
sometimes written "vLink". Official store (JS-rendered, product pages indexed under
`/products-detail/i-NN/`): https://www.vgatemall.com/

Current lineup per the official store and download center:

| Model | Radio | Notes | Source |
|---|---|---|---|
| vLinker MS | Classic BT (MFi) | "coming soon", 3 Mbps, iOS tested | vgatemall i-79 |
| vLinker FS (BT) | Classic BT (Apple MFi) + Windows/Android | FORScan HS/MS-CAN auto-switch; firmware `vLinker_FS_BT_2.3.22.zip` | vgatemall i-52, download center; Car Scanner ("vLinker FS with Bluetooth MFi") |
| vLinker FS (USB) | wired USB | FORScan; firmware `vLinker_FS_USB_v2.3.04.zip` | vgatemall i-19 |
| vLinker MC | Classic BT 3.0/4.0 | Android/Windows only | Amazon/AliExpress listings |
| vLinker MC+ | **BLE 4.0 + WiFi** | iOS+Android+Windows, "ELM327 V2.2" | vgatemall i-5; Car Scanner ("vLinker MC+ with Bluetooth LE") |
| vLinker BM / BM+ | Classic BT / **BLE 4.0 + WiFi** | BMW/BimmerCode, "ELM327 V2.2" | vgatemall i-15/i-16 |
| vLinker FD (WiFi) | WiFi | FORScan (Android/Windows) | vgatemall i-30 |
| **vLinker FD+** | **BLE 4.0** (iOS/Android/Windows) | FORScan/Mazda IDS; "ELM327 V2.2"; AT + ST command sets; MS-CAN | vgatemall i-23, gendan VGTEFD |

**"FS+" finding:** a targeted search ("vLinker FS+" with exclusions, and "vLinker FS+"
+ amazon) returns only `FS` (BT and USB) products — no distinct FS+ SKU on the official
store, the download center, or retailers. Report it as a reseller artifact.


## 2. vLinker FD+ — official spec and BLE behavior

From the official product page (https://www.vgatemall.com/products-detail/i-23):

- "Supports IOS/Android/Windows", "Made for Forscan", "Automatic Sleep & Wake-Up",
  "Compatible with **AT and ST command sets**", "Free firmware upgrade".
- "Vgate vLinker FD+ **ELM327 V2.2** ... Bluetooth for Mazda VCM2 IDS ... **MS CAN**".
- Protocols: "15 ELM format protocols; 23 STN format protocols; up to 64 user-defined
  protocols"; the 5 standard OBD-II protocols + SAE J1939, ISO 11898 raw CAN, MS-CAN.
- Bluetooth operation steps (Android): search for **"vLinker FD" or "vLinker FD-Android"**
  and Pair with code **1234**; "Attention!! Do NOT pair 'vLinker FD+' or 'vLinker FD-IOS'".
- iOS steps: "No need to configure Bluetooth device name in the phone. Just enable
  Bluetooth ... and continue to the OBD app setting." (i.e. pure BLE, app-level connect,
  no OS pairing).
- Box contents: "1 x Vgate vLinker FD+ **BT4.0**".

Interpretation for this repo: the FD+ presents two radio personalities — a Classic BT
SPP side (Android/Windows, PIN 1234) and a **BLE side (iOS)** that is unauthenticated
and app-connectable. The ESP32-S3 has no Classic BT, so only the BLE side applies.

## 3. BLE GATT profile evidence (UUID table)

Vgate does not publish UUIDs (repo README already notes this). Evidence below comes from
open-source projects that connect to vLinker/Vgate adapters over BLE on real hardware.

| Role | UUID (16-bit base `0000xxxx-0000-1000-8000-00805f9b34fb`) | Observed in |
|---|---|---|
| Service (primary candidate) | **`0x18F0`** | FocusDash_ESP32 (vLinker FD+, ESP32-S3 NimBLE central); OBD2bridge (ESP32-S3, "BLE ELM327 / Vgate / vLinker style"); tronikos elm327_obdii ("Do NOT skip 0x18F0 - that's a common OBD-II adapter service UUID"); obd2-dashboard WebBLE service list |
| Write/TX char | **`0x2AF1`** | FocusDash_ESP32 `BLE_WRITE_UUID`; OBD2bridge |
| Notify/RX char | **`0x2AF0`** | FocusDash_ESP32 `BLE_NOTIFY_UUID`; OBD2bridge |
| Service (alternative) | **`0xFFF0`** | FocusDash_iOS_Swift `elmServiceUUID` (same FD+ hardware, CoreBluetooth); ESP32OBDGauge; esphome-obd2-ble (Veepeak OBDCheck BLE+, ESP32-S3); tronikos defaults |
| Write/TX char | **`0xFFF2`** | FocusDash_iOS_Swift; ESP32OBDGauge; esphome-obd2-ble; tronikos default `DEFAULT_UUID_WRITE` |
| Notify/RX char | **`0xFFF1`** | FocusDash_iOS_Swift; ESP32OBDGauge; esphome-obd2-ble; tronikos default `DEFAULT_UUID_READ` |
| Service (HM-10 style) | **`0xFFE0`** (char `0xFFE1`) | obd2-dashboard WebBLE service list (common on cheap BLE ELM dongles) |
| Service (custom alt) | **`E7810A71-73AE-499D-8C15-FAA9AEF0C3F2`** | FocusDash_ESP32 `BLE_ALT_SERVICE_UUID`; FocusDash_iOS_Swift `altServiceUUID` |
| Char (custom alt, write+read same UUID) | **`BEF8D6C9-9C21-4C9E-B632-BD58C1009F9F`** | FocusDash_ESP32 `BLE_ALT_CHAR_UUID`; FocusDash_iOS_Swift `altWriteCharUUID`/`altReadCharUUID` |

**Nordic UART (`6e400001-b5a3-f393-e0a9-e50e24dcca9e`): no evidence** that any Vgate
adapter exposes it. None of the eight projects examined use it for vLinker/Veepeak
devices.

**Advertisement name:** Vgate BLE adapters advertise a name containing "VLINK" — the

## 4. ELM327-over-BLE serial protocol

The BLE service is a transparent serial pipe to the ELM327 engine. Request-reply
framing (all projects agree):

1. Write `ATZ\r` (with or without response — vLinker accepts **write-without-response**)
   to the TX char. The adapter reboots (~1 s).
2. Replies arrive as **notifications** on the RX char; the ELM327 prints the response
   then a prompt `>` — that prompt is the "ready for next command" signal.
3. Pace one command in flight; fire the next poll on `>` (plus ~20 ms settling delay
   observed on vLinker: "We add a tiny delay to ensure the serial buffer on the vLinker
   is actually clear."). Adapter-limited rate is ~10–20 Hz on a vLinker at 500 k CAN
   (FocusDash OBDManager.h).

Initialization sequence used by working vLinker clients (FocusDash iOS, ESP32; OBD2bridge):

```
ATZ          # reset; wait ~1.5 s
ATE0         # echo off
ATL0         # linefeeds off
ATS0         # spaces off
ATH0         # headers off (or ATH1 if the parser wants headers)
ATCAF1       # CAN auto-formatting on
ATST0A       # set timeout 40 ms (low-latency HS-CAN; iOS uses ATST1E=120 ms first)
ATAT2        # adaptive timing type 2 (aggressive)  [iOS only]
ATSP6        # protocol 6 = ISO 15765-4 CAN (11-bit, 500 k)  [vLinker HS-CAN path]
ATSP0        # or auto-detect
```

Mode-01 PID queries (hex over the serial pipe, e.g. `010C\r`):

| Query | Example response | Decode | Source for decode |
|---|---|---|---|
| `010C` (RPM) | `41 0C 1A F8` | `((0x1A<<8)|0xF8)/4` = 1695 rpm | Wikipedia OBD-II PIDs; FocusDash `parseCombinedRPMAndSpeed` (`rawRpm/4`, searches for `410C`) |
| `0105` (coolant) | `41 05 5A` | `0x5A − 40` = 50 °C | Wikipedia OBD-II PIDs |
| `010D` (speed) | `41 0D 64` | `0x64` = 100 km/h | FocusDash (2 hex digits after `410D`) |
| `0100` (supported PIDs) | `41 00 BE 3F A8 13` | bitmap, 4 bytes | Wikipedia |

Response parsing notes: with echo off the response may still contain the echoed command
if `ATE1` was left; parse by locating `41 0C` / `410C` inside the buffer rather than
assuming a fixed offset. With spaces on (`ATS1`) the same data appears as
`41 0C 1A F8`; with spaces off, `410C1AF8`.

## 5. ESP32-S3 NimBLE central — implementation plan

ESP32-S3 is BLE-only (no BR/EDR), so only the BLE path applies
(https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/bluetooth/index.html).
The NimBLE-Arduino API used below is documented at
https://h2zero.github.io/NimBLE-Arduino/ (client examples: `examples/NimBLE_Client`).
The exact call sequence is taken from two working ESP32-S3 central implementations
(FocusDash_ESP32 `OBDManager.cpp`; OBD2bridge `obd2bridge_gps_webpanel.ino`).

```cpp
// 1. Init — no security. FocusDash: forcing pairing made the link silent.
NimBLEDevice::init("boost-gauge");
NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
NimBLEDevice::setSecurityAuth(false, false, false);

// 2. Scan — match service UUID advertisement and/or name containing VLINK/OBD/ELM.
//    FocusDash matches case-insensitively (advertised name observed: "IOS-Vlink").
class ScanCB : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* adv) override {
    std::string name = adv->getName();
    if (adv->isAdvertisingService(NimBLEUUID("18F0")) ||
        adv->isAdvertisingService(NimBLEUUID("FFF0")) ||
        adv->isAdvertisingService(altServiceUUID) ||
        upper(name).find("VLINK") != npos || upper(name).find("OBD") != npos) {
      NimBLEDevice::getScan()->stop();
      queueConnect(adv->getAddress());
    }
  }
};
NimBLEDevice::getScan()->setAdvertisedDeviceCallbacks(new ScanCB());
NimBLEDevice::getScan()->start(10, scanCompleteCB /* resets _isScanning on timeout */);

// 3. Connect — do NOT force connection parameters. FocusDash: "forced low-latency

## 6. Bench test WITHOUT a car

The adapter must be powered: OBD pin 16 (battery +) and grounds pins 4/5 — a 12 V bench
supply on the OBD connector works. Vgate's "Automatic Sleep & Wake-Up" means the adapter
may be asleep until a BLE link/command arrives. **Beware the reverse-polarity risk and
fuse the bench supply; do not exceed ~16 V.**

AT commands that return data from the adapter itself (work on the bench):

| Command | Expected (typical) | Notes / source |
|---|---|---|
| `ATZ` | `ELM327 v2.2` then `>` | reset; ELM327 datasheet AT Z; Vgate markets "ELM327 V2.2". **Verify exact string on the bench unit.** |
| `ATI` | version ID string | ELM327 datasheet AT I (real chip prints `ELM327 v1.4b`; clones print their own) |
| `AT@1` | `OBDII to RS232 Interpreter` | ELM327 datasheet AT @1 (device description) |
| `ATRV` | e.g. `12.4V` | ELM327 datasheet AT RV; needs the 12 V bench supply; this is how the HA integration detects ignition on/off (tronikos) |
| `ATDP` / `ATDPN` | `AUTO` / `A0` | current protocol; auto = `A0` after `ATSP0` (ELM327 datasheet) |
| `ATSP0` | `OK` | set auto protocol (ELM327 datasheet) |
| `ATE0`/`ATL0`/`ATS0`/`ATH0`/`ATCAF1`/`ATST0A` | `OK` each | init set used by all vLinker clients |
| `AT@2` | device identifier (may be blank) | ELM327 datasheet |

Mode-01 PIDs on the bench (no ECU on the bus) — expect an error, not data:

| Command | Expected without a car |
|---|---|
| `0100`, `010C`, `0105`, `0120`... | `SEARCHING...` then `NO DATA` (or `CAN ERROR` on some units; ELM327 datasheet "NO DATA"/"CAN ERROR") |
| `0902` (VIN), `03` (DTCs), `02` (freeze frame) | `NO DATA` / `UNABLE TO CONNECT` |

**Freeze-frame question:** the ELM327 has no adapter-side data store. Mode 02 reads the

Bench script sketch (pseudocode; run once per connect):

```
write("ATZ\r")                 # expect: <version line> ">"   (allow ~2 s)
write("ATE0\r")                # expect: "OK" ">"
write("ATL0\r")                # "OK"
write("ATS0\r")                # "OK"
write("ATH0\r")                # "OK"
write("ATSP0\r")               # "OK"  (auto protocol)
write("ATI\r")                 # expect: "ELM327 v2.2" (verify exact string)
write("AT@1\r")                # expect: "OBDII to RS232 Interpreter"
write("ATRV\r")                # expect: "12.xV"  (proves 12 V bench feed)
write("ATDPN\r")               # expect: "A0" (auto)
write("010C\r")                # expect: "NO DATA" (no ECU) — link is healthy
write("0105\r")                # expect: "NO DATA"
# PASS criteria: every command above returned a prompt ">" within timeout;
# AT self-tests returned sane strings; PIDs returned NO DATA (not timeout/empty).
# If ATI/AT@1/ATRV return nothing, the wrong TX/RX chars were picked — log the
# discovered service/char UUIDs and try the FFF0 or E7810A71 profile.
```

What still needs the bench unit (unverifiable from here):
1. The exact ATI/ATZ strings of a real FD+ (vendor says "ELM327 V2.2").

## 7. Sources

- Vgate official store, FD+ product page (specs, pairing steps, AT+ST, ELM327 V2.2):
  https://www.vgatemall.com/products-detail/i-23
- Vgate official store, homepage (lineup, no FS+): https://www.vgatemall.com/
- Vgate download center (firmware zips incl. vLinker_FS_BT_2.3.22, vLinker_FD_V2.2.92):
  https://www.vgatemall.com/downloadcenter/
- Gendan (UK), vLinker FD+ 4.0 for FORScan: https://m.gendan.co.uk/product_VGTEFD.html
- Car Scanner adapter guide (vLinker MC+ = BLE LE, vLinker FS = MFi classic):
  https://carscanner.info/choosing-obdii-adapter/
- FocusDash_ESP32 (ESP32-S3 NimBLE central, connects to vLinker FD+; UUIDs in
  `src/config.h`; "IOS-Vlink" name; no-security note; write-without-response; `>`
  pacing; ~10–20 Hz): https://github.com/seobohdanov/FocusDash_ESP32
- FocusDash_iOS_Swift (CoreBluetooth central, same FD+; FFF0/FFF1/FFF2 + alt UUIDs;
  vLinker init ATSP6/ATH0/ATCAF1/ATST0A; 20 ms vLinker settle):
  https://github.com/seobohdanov/FocusDash_iOS_Swift
- OBD2bridge (ESP32-S3; service 18F0, write 2AF1, notify 2AF0; init
  ATZ/ATE0/ATL0/ATS0/ATH1/ATSP6): https://github.com/JMuffin/OBD2bridge
- elm327_obdii_ble (HA custom component; default FFF1 read/FFF2 write; auto-discovery):
  https://github.com/tronikos/elm327_obdii_ble
- elm327_obdii (transport_ble.py: dynamic fallback discovery, "do not skip 0x18F0"):
  https://github.com/tronikos/elm327_obdii
- obd2-dashboard (Web Bluetooth; known-service list ffe0/fff0/18f0/ffe5; first-write /
  first-notify fallback): https://github.com/yubun241/obd2-dashboard
- ESP32OBDGauge (service FFF0, write FFF2, notify FFF1):
  https://github.com/alonergan/ESP32OBDGauge
- esphome-obd2-ble (ESP32-S3 + Veepeak BLE+; FFF0/FFF2/FFF1):
  https://github.com/rubenmuehlhans/esphome-obd2-ble
- ELM327 datasheet (AT I / AT @1 / AT RV / AT SP / AT DPN / NO DATA / CAN ERROR /
  SEARCHING / non-volatile protocol memory):
  https://www.elmelectronics.com/wp-content/uploads/2016/07/ELM327DSH.pdf
- OBD-II PIDs (mode 01; PID 05 coolant `A−40`; PID 0C RPM `((A·256)+B)/4`):
  https://en.wikipedia.org/wiki/OBD-II_PIDs
- NimBLE-Arduino docs: https://h2zero.github.io/NimBLE-Arduino/
- ESP32-S3 Bluetooth (BLE-only, no BR/EDR):
  https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/bluetooth/index.html
- FORScan forum ("VLinker FD+ is a generic adapter, depending on your use, it will
  work"; FORScan recommends OBDLink/vLinker): https://forum.forscan.org/

2. Which of 18F0/2AF1/2AF0 vs FFF0/FFF1/FFF2 vs E7810A71… the FD+ actually exposes
   (two independent ESP32 projects and one iOS app agree it is one of these; the bench
   unit should dump its service table with nRF Connect or the firmware's own discovery
   log).
3. Whether the FD+ advertises the service UUID in the advertisement (scan filter) or
   only after connect (discovery-only match).
4. TPMS path (needs the car): the repo's `boost_tpms_protocol.c` implements the
   FORScan-style MS-CAN UDS read for the MX-5 ND; the FD+ is the FORScan-recommended
   adapter class that exposes MS-CAN, so the BLE transport + ELM327 framing verified on
   the bench is the same transport the TPMS queries will ride.

ECU's freeze-frame data from the vehicle bus; the only thing the adapter itself
remembers is the last protocol (non-volatile memory, AT M0/M1, ELM327 datasheet). So
**no, the vLinker does not expose a "freeze" of last data** — without a car there is
nothing to read back. (Same answer for all ELM327-family adapters.)

//    params on ESP32 produced a silent link"; default CoreBluetooth params work.
NimBLEClient* c = NimBLEDevice::getDisconnectedClient(); // or createClient()
c->setClientCallbacks(new ClientCB(), false);
c->setConnectTimeout(10);
if (!c->connect(addr)) { /* retry later */ }

// 4. Discover + pick characteristics (runtime discovery per README).
auto* services = c->getServices(true);          // log every service UUID
NimBLERemoteService* svc = c->getService("18F0");
if (!svc) svc = c->getService("FFF0");
if (!svc) svc = c->getService(altServiceUUID);  // E7810A71-...
NimBLERemoteCharacteristic* tx = svc->getCharacteristic("2AF1");
NimBLERemoteCharacteristic* rx = svc->getCharacteristic("2AF0");
if (!tx) tx = svc->getCharacteristic("FFF2");
if (!rx) rx = svc->getCharacteristic("FFF1");
if (!tx || !rx) { /* alt custom char BEF8D6C9-... as last resort */ }

// 5. Subscribe to NOTIFICATIONS (indications only as a fallback).
rx->subscribe(true, notifyCB /* appends bytes to an RX ring buffer */);

// 6. Write — without response (vLinker accepts it; avoids an ATT ack per request).
tx->writeValue((uint8_t*)"010C\r", 5, /*response=*/false);

// 7. Notify callback: accumulate bytes; a complete reply ends with '>'.
//    Parse "41 0C" inside the accumulated buffer; clear on '>'; send next command.
```

Robustness notes from the field:
- The general "find any serial-ish GATT" fallback (tronikos `transport_ble.py`, WebBLE
  `_initAfterConnect`) skips standard SIG services (0x1800, 0x1801, 0x180A, 0x180F) and
  picks the **first writable** char as TX and the **first notify/indicate** char as RX.
  tronikos explicitly warns not to skip 0x18F0 (it is the OBD service).
- After connect, log every discovered service/characteristic (FocusDash does) — that is
  how the bench unit will confirm which profile this specific adapter exposes.
- NimBLE on ESP32 runs scan + central connection + advertising concurrently
  (FocusDash gateway mode), so the existing gauge's BLE peripheral usage (if any) and
  this central role can coexist; Wi-Fi + BLE coexistence was already proven in this
  repo's `2026-07-26-ble-wifi-coexistence.md` spike.

FocusDash scan matches case-insensitively for `VLINK`/`OBD`/`ELM` because "vgate's BLE
adapters advertise as 'IOS-Vlink' (not 'VLinker')". The vendor's own docs name the BLE
identities `vLinker FD+` / `vLinker FD-IOS`. Do not rely on the exact name: match on
service UUID advertisement and/or case-insensitive `VLINK` substring, then verify by
service discovery after connect.

**Security/pairing:** "vLinker FD-IOS exposes an **unauthenticated UART-like GATT
service**. Forcing pairing/bonding from ESP32 made the adapter stay silent in-car, so
keep the link plain unless the peripheral explicitly asks for it." (FocusDash
OBDManager.cpp `begin()`). No passkey is needed; if a passkey request still arrives,
FocusDash's defensive callback returns `000000`.

**OBDKonnect finding:** `obdkonnect.com` is unreachable and has **zero** Wayback
captures (CDX query returns `[]`). No evidence exists that OBDKonnect brands or sells
vLinker products. Vgate's official UK agents are BMDiag/prestigediagnostics.uk and
Gendan (which sells the FD+ as `VGTEFD`).
