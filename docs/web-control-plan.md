# Boost Gauge Web Control (WLED-style)

## Decision
Ship a **captive SoftAP + HTTP control plane** first. Native Android/iOS apps can wrap the same REST/SSE later.

## Goals (v0.2.0)
1. SoftAP `BoostGauge-XXXX` (open or simple password)
2. Web UI at `http://192.168.4.1/`
3. Live PSI mirror (SSE)
4. Brightness + peak reset
5. Theme select (at least 2 themes: night black, ghost gray)
6. Time sync + auto-dim schedule
7. OTA firmware upload (multipart) with dual app slots
8. GIF upload stub (store on SPIFFS/SD later; API present)

## Non-goals (this pass)
- BLE
- Full MAP sensor path
- Android/iOS apps
- Signed OTA

## Architecture

```
app_main
  ├─ display + gauge task (existing)
  ├─ boost_config (NVS)
  ├─ boost_wifi_ap (SoftAP)
  ├─ boost_http (httpd + static + REST + SSE)
  └─ boost_ota (multipart write to next OTA slot)
```

## Partition table (16 MB)
| Name     | Type | SubType | Size |
|----------|------|---------|------|
| nvs      | data | nvs     | 24K  |
| otadata  | data | ota     | 8K   |
| phy_init | data | phy     | 4K   |
| ota_0    | app  | ota_0   | 3M   |
| ota_1    | app  | ota_1   | 3M   |
| storage  | data | spiffs  | ~10M |

App size today ~0.8 MB; 3M slots leave headroom.

## REST API
| Method | Path | Body / notes |
|--------|------|----------------|
| GET | `/api/status` | JSON: version, psi, peak, brightness, theme, wifi, uptime, time |
| GET | `/api/stream` | SSE: `data: {psi,peak,zone,brightness}\n\n` ~10 Hz |
| POST | `/api/brightness` | `{"percent":0-100}` or `{"toggle":true}` |
| POST | `/api/peak/reset` | empty |
| GET/POST | `/api/config` | full config JSON (theme, dim schedule, units) |
| POST | `/api/time` | `{"epoch":1710000000,"tz_offset_min":-300}` |
| POST | `/api/ota` | `multipart/form-data` file field `firmware` |
| GET | `/api/themes` | list theme ids + labels |
| POST | `/api/theme` | `{"id":"night_black"}` |
| POST | `/api/gif` | multipart stub → 501 or store to SPIFFS if ready |

## Config schema (NVS namespace `boost`)
```c
typedef struct {
  uint8_t  brightness;      // 0-100
  uint8_t  theme_id;        // enum
  uint8_t  dim_enable;      // 0/1
  uint8_t  dim_start_hour;  // 0-23
  uint8_t  dim_start_min;
  uint8_t  dim_end_hour;
  uint8_t  dim_end_min;
  int16_t  tz_offset_min;
  uint8_t  units;           // 0=psi
  char     ap_pass[16];     // optional
} boost_config_t;
```

## Themes (v1)
| id | name | face |
|----|------|------|
| 0 | Night Black | `#000000` (current) |
| 1 | Ghost Gray | `#1A1D24` (FACE_BG standby) |

Theme apply: set `FACE_BG` at runtime via `boost_gauge_set_theme()`.

## Web UI pages (single SPA-ish HTML)
- Live gauge canvas (mirror)
- Brightness slider + dim toggle
- Theme chips
- Time sync button (uses browser clock)
- Dim schedule form
- OTA file upload
- Status footer (version, IP, free heap)

Static assets embedded via `esp_embed` / CMake `EMBED_FILES` or SPIFFS image.

## SoftAP
- SSID: `BoostGauge-` + last 4 of MAC
- IP: 192.168.4.1
- Channel 1, max 4 clients
- Optional open network for first boot (document security)

## Threading
- httpd on default stack (8192+)
- SSE task or httpd async
- All LVGL calls under `bsp_display_lock`
- Config writes under mutex; apply brightness immediately

## Build verification
- `idf.py build` succeeds
- Host sim still builds (no ESP web code required on host)
- Document flash + connect steps in README
