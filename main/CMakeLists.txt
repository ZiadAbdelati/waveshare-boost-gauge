# OBD2 BLE central: the source files are gated on BOOST_TPMS_BLE_ENABLED, but
# the `bt` component is REQUIRED unconditionally. main's CMakeLists runs twice
# (a discovery pass before Kconfig is populated, then the real pass); a
# CONFIG_-gated REQUIRES is evaluated empty in the first pass and that empty
# registration wins, stranding the NimBLE include dirs. bt contributes nothing
# to the binary when CONFIG_BT_ENABLED=n.
if(CONFIG_BOOST_TPMS_BLE_ENABLED)
    set(OBD_SRCS "boost_obd_ble.c" "boost_obd_elm.c" "boost_obd.c")
else()
    set(OBD_SRCS "boost_obd_stub.c")
endif()

idf_component_register(
    SRCS
        "main.c"
        "boost_display.c"
        "boost_gauge.c"
        "boost_page.c"
        "boost_tpms_ui.c"
        "boost_tpms.c"
        "boost_tpms_mock.c"
        "boost_tpms_protocol.c"
        "boost_model.c"
        "boost_sim.c"
        "boost_sensors.c"
        "boost_brightness.c"
        "boost_theme.c"
        "boost_neon_geom.c"
        "boost_web.c"
        "boost_media_store.c"
        "boost_network.c"
        ${OBD_SRCS}
        "generated_web_assets.c"
        # Vendored GIF playback layer. The upstream copies under
        # managed_components/lvgl__lvgl/src/libs/gif/ are excluded from the
        # build in the top-level CMakeLists.txt; see main/gif/boost_gif.c.
        "gif/boost_gif.c"
        "gif/boost_gif_dec.c"
        "fonts/alvida_big.c"
        "fonts/font_mono_16.c"
        "fonts/font_mono_40.c"
        "fonts/font_cond_14.c"
        "fonts/font_cond_18.c"
        "fonts/font_cond_22.c"
        "fonts/font_cond_32.c"
        "fonts/font_cond_96.c"
        "fonts/archivo_black_56.c"
        "fonts/neon_big.c"
        "fonts/neon_label.c"
        "fonts/font_wide_22.c"
        "fonts/font_wide_32.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        app_update
        esp_event
        espressif__esp_lcd_co5300
        esp_driver_spi
        esp_driver_i2c
        esp_http_server
        esp_netif
        esp_partition
        esp_timer
        esp_wifi
        json
        nvs_flash
        bt
)
