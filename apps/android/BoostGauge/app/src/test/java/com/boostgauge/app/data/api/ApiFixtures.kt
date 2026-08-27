package com.boostgauge.app.data.api

/**
 * Fixture payloads copied from the firmware handler output shapes in
 * main/boost_web.c (state_json, config_json, themes_get, sensors_calibration_json,
 * logs_get, tpms_config_json) and main/boost_theme.c theme list.
 */
object ApiFixtures {

    val STATE = """
        {
          "psi": 3.42, "peakPsi": 5.01, "zone": "BOOST", "demo": true,
          "brightness": 92, "firmwareVersion": "v0.8.0", "uptimeMs": 1234567,
          "epochMs": 1780000000000, "timezoneOffsetMinutes": -240,
          "activeThemeId": "neon", "activePage": 0,
          "display": {
            "renderFps": 60, "gaugeDemandPerSecond": 60, "flushesPerSecond": 60,
            "pixelsPerSecond": 1000000, "worstRenderUs": 8500,
            "renderGapP50Us": 400, "renderGapMaxUs": 20000,
            "framesOverBudget": 0, "tePeriodUs": 16667, "teWaits": 60,
            "teTimeouts": 0, "teSkips": 0, "teScanlineWaits": 0
          },
          "sensors": {
            "adsPresent": false, "bmpPresent": false, "fault": false,
            "mapVolts": 0.0, "mapAbsKpa": 0.0, "ambientKpa": 0.0
          },
          "tpms": {
            "status": 0, "lowPsi": 31.9,
            "wheels": [
              {"psi": 32.5, "valid": true}, {"psi": 33.0, "valid": true},
              {"psi": 31.8, "valid": true}, {"psi": 32.8, "valid": true}
            ]
          },
          "obd": {
            "state": 3, "lastError": 0, "peer": "vlinker fd+",
            "peerAddr": "11:22:33:44:55:66", "uptimeMs": 1234567, "ageMs": 120,
            "valid": true, "lastReply": "010C='410C 0A 00'",
            "protocol": "ISO 15765-4 (CAN 11/500)", "rpm": 2952.0,
            "speedKph": 0.0, "coolantC": 88.0, "mapKpa": 56.6, "iatC": 31.0,
            "throttlePct": 18.0, "mafGps": 5.2, "fuelPct": 62.0, "batteryV": 12.4
          }
        }
    """.trimIndent()

    val CONFIG = """
        {
          "brightnessHigh": 92, "brightnessLow": 10,
          "dimSchedule": {"enabled": true, "startMinutes": 1380, "endMinutes": 360},
          "timezoneOffsetMinutes": -240,
          "timezoneTz": "EST5EDT,M3.2.0/2,M11.1.0/2",
          "activeThemeId": "dyno-cell",
          "psiMin": -15.0, "psiMax": 10.0, "psiOverboost": 8.0, "zeroAngle": 90.0,
          "appBle": false
        }
    """.trimIndent()

    val THEMES = """
        {
          "activeThemeId": "dyno-cell",
          "bigDigitStaticBg": true, "bigDigitColorText": true,
          "bigDigitStaticColor": "#16181c", "bigDigitTextColor": "#ffffff",
          "arcGradient": false, "hudGradient": false, "hudTrueBlack": false,
          "neonMarqueeSpin": false, "teSync": false, "regionDBuf": true,
          "teScanline": false, "rotation": 0, "vaultFace": "#05281a",
          "vaultVignette": 22, "vaultNeedleRed": false, "vaultNeedleTail": false,
          "neonLayout": 0, "neonPreset": 0, "demoMode": false,
          "demoFastSweep": false, "tpmsBle": false, "pixelShift": false,
          "pixelShiftSec": 90,
          "themes": [
            {
              "id": "dyno-cell", "name": "Dyno Cell", "style": "arc",
              "colors": {"face": "#090a0d", "track": "#20242c", "text": "#f5f7fa",
                "muted": "#8c95a3", "vacuum": "#4dd2ff", "boost": "#b8f35a",
                "overboost": "#ff4f6d", "zero": "#ffffff"},
              "customized": false
            },
            {
              "id": "vault-tec", "name": "Vault-Tec", "style": "vault",
              "colors": {"face": "#05281a", "track": "#0c3d24", "text": "#38f08a",
                "muted": "#1f7a4d", "vacuum": "#38f08a", "boost": "#38f08a",
                "overboost": "#eafc50", "zero": "#38f08a"},
              "customized": false
            },
            {
              "id": "night-city", "name": "Night City", "style": "hud",
              "colors": {"face": "#080a08", "track": "#1a1c0a", "text": "#fcee0a",
                "muted": "#5a7a0a", "vacuum": "#00e5ff", "boost": "#fcee0a",
                "overboost": "#ff003c", "zero": "#00e5ff"},
              "customized": true
            },
            {
              "id": "big-digit", "name": "Big Digit", "style": "bigdigit",
              "colors": {"face": "#0b0c0e", "track": "#20242c", "text": "#ffffff",
                "muted": "#0b0c0e", "vacuum": "#4dd2ff", "boost": "#b8f35a",
                "overboost": "#ff4f6d", "zero": "#ffffff"},
              "customized": false
            },
            {
              "id": "neon", "name": "Neon", "style": "neon",
              "colors": {"face": "#000000", "track": "#241038", "text": "#ffffff",
                "muted": "#5a3a7a", "vacuum": "#7b00ff", "boost": "#ff2bd6",
                "overboost": "#ff1500", "zero": "#ffffff"},
              "customized": false
            }
          ]
        }
    """.trimIndent()

    val CALIBRATION = """
        {
          "supplyVolts": 4.9860,
          "live": {
            "adsPresent": true, "bmpPresent": true, "fault": false,
            "mapVolts": 1.1821, "mapAgeMs": 42, "nominalKpa": 101.32,
            "correctedKpa": 101.32, "bmpKpa": 101.30, "bmpAgeMs": 41,
            "bmpUpdates": 12345, "ambientIsFallback": false
          },
          "calibration": {
            "valid": true, "version": 3, "offsetKpa": 0.02, "offsetPsi": 0.003,
            "supplyVolts": 4.9860, "refMapVolts": 1.1818,
            "refNominalKpa": 101.28, "refBmpKpa": 101.30, "samples": 60,
            "epochMs": 1780000000000
          }
        }
    """.trimIndent()

    val LOGS = """
        {
          "samples": [
            {"tMs": 1000, "psi": 0.12, "peakPsi": 0.34, "zone": "ATMO", "demo": true},
            {"tMs": 1200, "psi": 1.02, "peakPsi": 1.02, "zone": "BOOST", "demo": true}
          ]
        }
    """.trimIndent()

    val TPMS_CONFIG = """
        {"lowKpa": 220.0, "lowPsi": 31.9, "staleAfterMs": 15000}
    """.trimIndent()

    val ERROR_CLOCK_REJECTED = """{"error":"clock_rejected"}"""
}
