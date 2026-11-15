// Thermostat test firmware v2 for the Guition JC4880P443C (ESP32-P4).
// Boot: display -> thermostat task (runs from here on, core 1) -> hardware check -> Wi-Fi/clock/web -> UI.
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "esp_lvgl_port.h"
#include "board.h"
#include "ui.h"
#include "ui_theme.h"
#include "settings.h"
#include "sht4x.h"
#include "ot_bus.h"
#include "thermostat.h"
#include "selftest.h"
#include "timekeep.h"
#include "webui.h"
#include "debug.h"
#include "motion.h"

static const char *TAG = "main";

// After an OTA update the new image is "pending verify": keep it only if the hardware check passes.
static void ota_confirm(bool hw_ok)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    if (hw_ok) {
        ESP_LOGI(TAG, "new firmware passed the hardware check: keeping it");
        esp_ota_mark_app_valid_cancel_rollback();
    } else {
        ESP_LOGE(TAG, "new firmware failed the hardware check: rolling back");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    settings_load();
    ESP_ERROR_CHECK(board_display_init());
    board_backlight(settings_get()->bright);

    lvgl_port_lock(0);
    ui_theme_init();  // dark until the day/night logic runs
    lvgl_port_unlock();

    // The thermostat starts before anything slow, so it runs even while the hardware check waits.
    sht4x_init(board_i2c_bus());
    otbus_start();  // OpenTherm cadence from here on
    thermo_start();

    bool hw_ok = selftest_run();  // also brings up audio, camera and Wi-Fi
    ota_confirm(hw_ok);

    timekeep_init();
    webui_start();

    lvgl_port_lock(0);
    ui_shell_create();
    if (!settings_wifi_configured()) {
        ui_open_setup();  // first run: set up Wi-Fi
    }
    lvgl_port_unlock();
    motion_start();
    debug_start();
    ESP_LOGI(TAG, "UI running");
}
