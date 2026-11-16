idf_component_register(
    SRCS "main.c" "board.c" "settings.c" "sht4x.c" "opentherm.c" "ot_frame.c" "ot_sim.c" "plant_sim.c" "otb_core.c" "ot_bus.c" "control.c" "cfgtable.c" "schedule.c" "datalog.c" "alerts.c" "service.c" "mqtt_ha.c" "thermostat.c" "timekeep.c"
         "camera.c" "ha.c" "selftest.c" "webui.c" "ui.c" "debug.c" "c6update.c" "hadash.c" "motion.c" "ui_theme.c" "quickpanel.c"
         "app_thermo.c" "font_room_75.c" "ui_pages.c" "cfgall.c" "cfgtext.c" "app_ha.c" "app_calc.c" "app_blocks.c" "app_setup.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_ledc esp_driver_gptimer
             esp_http_client esp_http_server esp_netif esp_wifi esp_event nvs_flash json mbedtls esp_timer
             esp_hw_support esp_psram spi_flash app_update esp_app_format esp_video quirc esp_driver_usb_serial_jtag mqtt)
