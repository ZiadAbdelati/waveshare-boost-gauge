#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Companion-app passkey display UI. Renders the six-digit LE pairing passkey
 * as a passive full-screen LVGL overlay (Montserrat-48 digits + a short hint),
 * following the AP-join QR overlay pattern in boost_page.c: objects are created
 * and deleted on the LVGL worker task under the display lock, on top of the
 * active screen that survives theme rebuilds; the overlay is dismissed by any
 * fresh tap, by pairing completion/failure (boost_app_ble pair-result hook), or
 * after ~60 s.
 *
 * Must be called after the display is started (LVGL worker running) and after
 * boost_app_ble_init(); idempotent. Registers this module's callbacks and
 * creates one LVGL poll timer — the callback bodies never touch LVGL.
 */
void boost_app_ble_ui_init(void);

#ifdef __cplusplus
}
#endif
