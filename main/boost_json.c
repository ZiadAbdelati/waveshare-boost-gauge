#include "boost_json.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "boost_app_ble.h"
#include "boost_model.h"
#include "boost_network.h"
#include "boost_sensors.h"
#include "boost_sim.h"
#include "boost_theme.h"
#include "boost_tpms.h"
#include "boost_tpms_protocol.h"

void boost_json_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    if (!in) {
        if (out_len) out[0] = '\0';
        return;
    }
    for (size_t i = 0; in[i] && o + 2 < out_len; ++i) {
        char c = in[i];
        if (c == '"' || c == '\\') {
            if (o + 3 >= out_len) break;
            out[o++] = '\\';
            out[o++] = c;
        } else if ((unsigned char)c < 0x20) {
            continue;
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
}

int boost_json_state(char *json, size_t len)
{
    boost_state_t st;
    boost_model_get_state(&st);
    boost_tpms_config_t tpms_cfg;
    boost_tpms_get_config(&tpms_cfg);
    return snprintf(json, len,
                    "{\"psi\":%.2f,\"peakPsi\":%.2f,\"zone\":\"%s\",\"demo\":%s,"
                    "\"brightness\":%d,\"firmwareVersion\":\"%s\",\"uptimeMs\":%llu,"
                    "\"epochMs\":%lld,\"timezoneOffsetMinutes\":%d,\"activeThemeId\":\"%s\",\"activePage\":%d,"
                    "\"display\":{\"renderFps\":%lu,\"gaugeDemandPerSecond\":%lu,\"flushesPerSecond\":%lu,\"pixelsPerSecond\":%lu,"
                    "\"worstRenderUs\":%lu,\"renderGapP50Us\":%lu,"
                    "\"renderGapMaxUs\":%lu,\"framesOverBudget\":%lu,"
                    "\"tePeriodUs\":%lu,\"teWaits\":%lu,\"teTimeouts\":%lu,\"teSkips\":%lu,"
                    "\"teScanlineWaits\":%lu},"
                    /* Raw sensor readings so a bench check against a known value
                     * (atmospheric ~101.3 kPa, gauge ~0 psi engine-off) is
                     * possible without the display. */
                    "\"sensors\":{\"adsPresent\":%s,\"bmpPresent\":%s,\"fault\":%s,"
                    "\"mapVolts\":%.4f,\"mapAbsKpa\":%.2f,\"ambientKpa\":%.2f},"
                    "\"tpms\":{\"status\":%d,\"lowPsi\":%.1f,\"wheels\":[{\"psi\":%.1f,\"valid\":%s},{\"psi\":%.1f,\"valid\":%s},{\"psi\":%.1f,\"valid\":%s},{\"psi\":%.1f,\"valid\":%s}]},"
                    "\"obd\":{\"state\":%d,\"lastError\":%u,\"peer\":\"%s\",\"peerAddr\":\"%s\",\"uptimeMs\":%lu,\"ageMs\":%lu,\"valid\":%s,"
                    "\"lastReply\":\"%s\",\"protocol\":\"%s\","
                    "\"rpm\":%.1f,\"speedKph\":%.1f,\"coolantC\":%.1f,\"mapKpa\":%.1f,\"iatC\":%.1f,"
                    "\"throttlePct\":%.1f,\"mafGps\":%.1f,\"fuelPct\":%.1f,\"batteryV\":%.1f}}",
                    (double)st.psi, (double)st.peak_psi, st.zone, st.demo ? "true" : "false",
                    st.brightness, st.firmware_version, (unsigned long long)st.uptime_ms,
                    (long long)st.epoch_ms, st.timezone_offset_minutes, st.active_theme_id, st.active_page,
                    (unsigned long)st.display.render_fps,
                    (unsigned long)st.display.gauge_demand_per_second,
                    (unsigned long)st.display.flushes_per_second,
                    (unsigned long)st.display.pixels_per_second,
                    (unsigned long)st.display.worst_render_us,
                    (unsigned long)st.display.render_gap_p50_us,
                    (unsigned long)st.display.render_gap_max_us,
                    (unsigned long)st.display.frames_over_budget,
                    (unsigned long)st.display.te_period_us,
                    (unsigned long)st.display.te_waits,
                    (unsigned long)st.display.te_timeouts,
                    (unsigned long)st.display.te_skips,
                    (unsigned long)st.display.te_scanline_waits,
                    st.ads_present ? "true" : "false",
                    st.bmp_present ? "true" : "false",
                    st.sensor_fault ? "true" : "false",
                    (double)st.map_volts, (double)st.map_abs_kpa, (double)st.ambient_kpa,
                    st.tpms_status, (double)boost_tpms_protocol_kpa_to_psi(tpms_cfg.low_kpa),
                    (double)st.tpms_psi[0], st.tpms_valid[0] ? "true" : "false",
                    (double)st.tpms_psi[1], st.tpms_valid[1] ? "true" : "false",
                    (double)st.tpms_psi[2], st.tpms_valid[2] ? "true" : "false",
                    (double)st.tpms_psi[3], st.tpms_valid[3] ? "true" : "false",
                    st.obd_state, (unsigned)st.obd_last_error, st.obd_peer, st.obd_peer_addr,
                    (unsigned long)st.obd_uptime_ms, (unsigned long)st.obd_age_ms,
                    st.obd_valid ? "true" : "false",
                    st.obd_last_reply, st.obd_protocol,
                    (double)st.obd_rpm, (double)st.obd_speed_kph, (double)st.obd_coolant_c,
                    (double)st.obd_map_kpa, (double)st.obd_iat_c,
                    (double)st.obd_throttle_pct, (double)st.obd_maf_gps,
                    (double)st.obd_fuel_pct, (double)st.obd_battery_v);
}

int boost_json_config(char *json, size_t len)
{
    boost_config_t cfg;
    boost_model_get_config(&cfg);
    return snprintf(json, len,
                    "{\"brightnessHigh\":%d,\"brightnessLow\":%d,"
                    "\"dimSchedule\":{\"enabled\":%s,\"startMinutes\":%d,\"endMinutes\":%d},"
                    "\"timezoneOffsetMinutes\":%d,\"timezoneTz\":\"%s\",\"activeThemeId\":\"%s\","
                    "\"psiMin\":%.2f,\"psiMax\":%.2f,\"psiOverboost\":%.2f,\"zeroAngle\":%.2f,"
                    "\"appBle\":%s}",
                    cfg.brightness_high, cfg.brightness_low,
                    cfg.dim_schedule.enabled ? "true" : "false",
                    cfg.dim_schedule.start_minutes, cfg.dim_schedule.end_minutes,
                    cfg.timezone_offset_minutes, cfg.timezone_tz, cfg.active_theme_id,
                    (double)cfg.psi_min, (double)cfg.psi_max, (double)cfg.psi_overboost,
                    (double)cfg.zero_angle, boost_app_ble_enabled() ? "true" : "false");
}

int boost_json_tpms_config(char *json, size_t len)
{
    boost_tpms_config_t cfg;
    boost_tpms_get_config(&cfg);
    return snprintf(json, len,
                    "{\"lowKpa\":%.1f,\"lowPsi\":%.1f,\"staleAfterMs\":%lu}",
                    (double)cfg.low_kpa,
                    (double)boost_tpms_protocol_kpa_to_psi(cfg.low_kpa),
                    (unsigned long)cfg.stale_after_ms);
}

static long long age_ms_json(uint32_t age_ms)
{
    /* UINT32_MAX is the firmware's "never read successfully" sentinel; emitting
     * it literally would read as a 49-day-old sample in the dashboard. */
    return age_ms == UINT32_MAX ? -1 : (long long)age_ms;
}

int boost_json_calibration(char *json, size_t len)
{
    const boost_sample_t s = boost_sensors_get_sample();
    const boost_map_cal_t cal = boost_sensors_get_calibration();
    const bool cal_valid = cal.version != 0;
    return snprintf(json, len,
                    "{\"supplyVolts\":%.4f,"
                    "\"live\":{\"adsPresent\":%s,\"bmpPresent\":%s,\"fault\":%s,"
                    "\"mapVolts\":%.4f,\"mapAgeMs\":%lld,\"nominalKpa\":%.2f,"
                    "\"correctedKpa\":%.2f,\"bmpKpa\":%.2f,\"bmpAgeMs\":%lld,"
                    "\"bmpUpdates\":%lu,\"ambientIsFallback\":%s},"
                    "\"calibration\":{\"valid\":%s,\"version\":%u,"
                    "\"offsetKpa\":%.2f,\"offsetPsi\":%.3f,\"supplyVolts\":%.4f,"
                    "\"refMapVolts\":%.4f,\"refNominalKpa\":%.2f,"
                    "\"refBmpKpa\":%.2f,\"samples\":%u,\"epochMs\":%lld}}",
                    (double)boost_sensors_get_supply_volts(),
                    s.ads_present ? "true" : "false",
                    s.bmp_present ? "true" : "false",
                    s.sensor_fault ? "true" : "false",
                    (double)s.map_volts, age_ms_json(s.ads_age_ms),
                    (double)boost_sensors_nominal_kpa(s.map_volts),
                    (double)s.map_abs_kpa, (double)s.ambient_kpa,
                    age_ms_json(s.bmp_age_ms), (unsigned long)s.bmp_updates,
                    s.ambient_is_fallback ? "true" : "false",
                    cal_valid ? "true" : "false", (unsigned)cal.version,
                    cal_valid ? (double)cal.offset_kpa : 0.0,
                    cal_valid ? (double)(cal.offset_kpa * 0.145037738f) : 0.0,
                    cal_valid ? (double)cal.supply_volts : 0.0,
                    cal_valid ? (double)cal.ref_map_volts : 0.0,
                    cal_valid ? (double)cal.ref_nominal_kpa : 0.0,
                    cal_valid ? (double)cal.ref_bmp_kpa : 0.0,
                    cal_valid ? (unsigned)cal.samples : 0u,
                    cal_valid ? (long long)cal.epoch_ms : 0LL);
}

int boost_json_network_status(char *json, size_t len)
{
    boost_net_status_t st;
    boost_network_get_status(&st);
    char ssid_e[96];
    char ap_e[64];
    boost_json_escape(st.sta_ssid, ssid_e, sizeof(ssid_e));
    boost_json_escape(st.ap_ssid, ap_e, sizeof(ap_e));
    size_t off = 0;
    int n = snprintf(json + off, len - off,
                     "{\"mode\":\"%s\",\"staEnabled\":%s,\"staConnected\":%s,"
                     "\"staSsid\":\"%s\",\"staIp\":\"%s\",\"apSsid\":\"%s\",\"apIp\":\"%s\","
                     "\"rssi\":%d,\"hasPassword\":%s,\"saved\":[",
                     st.mode == BOOST_NET_MODE_APSTA ? "apsta" : "ap",
                     st.sta_enabled ? "true" : "false",
                     st.sta_connected ? "true" : "false",
                     ssid_e, st.sta_ip, ap_e, st.ap_ip, st.rssi,
                     st.has_sta_pass ? "true" : "false");
    if (n < 0) {
        return n;
    }
    off += (size_t)n;
    for (uint8_t i = 0; i < st.saved_count && off < len; ++i) {
        char s_esc[96];
        boost_json_escape(st.saved[i].ssid, s_esc, sizeof(s_esc));
        n = snprintf(json + off, len - off, "%s{\"ssid\":\"%s\"}",
                     i == 0 ? "" : ",", s_esc);
        if (n < 0) {
            return n;
        }
        off += (size_t)n;
    }
    if (off + 2U < len) {
        (void)snprintf(json + off, len - off, "]}");
    }
    return (int)strlen(json);
}

int boost_json_theme_item(char *json, size_t len, const boost_theme_t *theme)
{
    return snprintf(json, len,
                    "{\"id\":\"%s\",\"name\":\"%s\",\"style\":\"%s\",\"colors\":{\"face\":\"#%06lx\","
                    "\"track\":\"#%06lx\",\"text\":\"#%06lx\",\"muted\":\"#%06lx\","
                    "\"vacuum\":\"#%06lx\",\"boost\":\"#%06lx\",\"overboost\":\"#%06lx\","
                    "\"zero\":\"#%06lx\"},"
                    "\"customized\":%s}",
                    theme->id, theme->name, boost_style_name(theme->style),
                    (unsigned long)theme->face, (unsigned long)theme->track,
                    (unsigned long)theme->text, (unsigned long)theme->muted,
                    (unsigned long)theme->vacuum, (unsigned long)theme->boost,
                    (unsigned long)theme->overboost, (unsigned long)theme->zero,
                    boost_theme_is_customized(theme->id) ? "true" : "false");
}

int boost_json_themes(char *json, size_t len)
{
    boost_config_t cfg;
    boost_model_get_config(&cfg);
    size_t off = 0;
    int n = snprintf(json + off, len - off,
                     "{\"activeThemeId\":\"%s\",\"bigDigitStaticBg\":%s,"
                     "\"bigDigitColorText\":%s,\"bigDigitStaticColor\":\"#%06lx\","
                     "\"bigDigitTextColor\":\"#%06lx\","
                     "\"arcGradient\":%s,\"hudGradient\":%s,\"hudTrueBlack\":%s,\"neonMarqueeSpin\":%s,"
                     "\"teSync\":%s,\"regionDBuf\":%s,\"teScanline\":%s,"
                     "\"rotation\":%u,"
                     "\"vaultFace\":\"#%06lx\",\"vaultVignette\":%u,\"vaultNeedleRed\":%s,"
                     "\"vaultNeedleTail\":%s,\"neonLayout\":%u,\"neonFont\":%u,\"neonPreset\":%u,\"demoMode\":%s,\"demoFastSweep\":%s,"
                     "\"tpmsBle\":%s,"
                     "\"pixelShift\":%s,\"pixelShiftSec\":%u,\"themes\":[",
                     cfg.active_theme_id,
                     boost_theme_bigdigit_static_bg() ? "true" : "false",
                     boost_theme_bigdigit_color_text() ? "true" : "false",
                     (unsigned long)boost_theme_bigdigit_static_color(),
                     (unsigned long)boost_theme_bigdigit_text_color(),
                     boost_theme_arc_gradient() ? "true" : "false",
                     boost_theme_hud_gradient() ? "true" : "false",
                     boost_theme_hud_true_black() ? "true" : "false",
                     boost_theme_neon_marquee_spin() ? "true" : "false",
                     boost_theme_te_sync() ? "true" : "false",
                     boost_theme_region_dbuf() ? "true" : "false",
                     boost_theme_te_scanline() ? "true" : "false",
                     (unsigned)boost_theme_rotation(),
                     (unsigned long)boost_theme_vault_face(),
                     (unsigned)boost_theme_vault_vignette_pct(),
                     boost_theme_vault_needle_red() ? "true" : "false",
                     boost_theme_vault_needle_tail() ? "true" : "false",
                     (unsigned)boost_theme_neon_layout(),
                     (unsigned)boost_theme_neon_font(),
                     (unsigned)boost_theme_neon_preset(),
                     boost_theme_demo_mode() ? "true" : "false",
                     boost_sim_fast_sweep() ? "true" : "false",
                     boost_theme_tpms_ble() ? "true" : "false",
                     boost_theme_pixel_shift() ? "true" : "false",
                     (unsigned)boost_theme_pixel_shift_sec());
    if (n < 0) {
        return n;
    }
    off += (size_t)n;
    for (size_t i = 0; i < boost_theme_count(); ++i) {
        if (i > 0) {
            if (off + 1U >= len) {
                return -1;
            }
            json[off++] = ',';
        }
        if (off >= len) {
            return -1;
        }
        n = boost_json_theme_item(json + off, len - off, boost_theme_at(i));
        if (n < 0 || (size_t)n >= len - off) {
            return -1;
        }
        off += (size_t)n;
    }
    if (off + 2U >= len) {
        return -1;
    }
    (void)snprintf(json + off, len - off, "]}");
    return (int)off + 2;
}
