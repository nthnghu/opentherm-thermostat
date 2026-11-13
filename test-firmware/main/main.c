#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_lvgl_port.h"
#include "board.h"
#include "ui.h"
#include "settings.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    settings_load();
    ESP_ERROR_CHECK(board_display_init());

    if (board_audio_init() != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed; Recorder will show an error");
    }
    if (board_wifi_start() != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi not started: %s", board_wifi_status());
    }

    lvgl_port_lock(0);
    lv_display_t *disp = lv_display_get_default();
    lv_theme_t *th = lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_TEAL),
                                           true, &lv_font_montserrat_20);
    lv_display_set_theme(disp, th);
    ui_home_create();
    if (!settings_wifi_configured()) {
        ui_open_setup();  // first run: set up Wi-Fi
    }
    lvgl_port_unlock();

    ESP_LOGI(TAG, "UI running");
}
