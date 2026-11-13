idf_component_register(
    SRCS "main.c" "board.c" "ui.c" "app_ha.c" "app_voice.c" "app_blocks.c" "app_setup.c" "settings.c" "portal.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_http_client esp_http_server
             esp_netif esp_wifi esp_event nvs_flash json mbedtls esp_timer esp_hw_support)
