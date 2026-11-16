// Thermostat test firmware v2 for the Guition JC4880P443C (ESP32-P4).
// Boot: display -> thermostat task (runs from here on, core 1) -> hardware check -> Wi-Fi/clock/web -> UI.
#include "esp_log.h"
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
#include "logring.h"
#include "ota_check.h"
#include "motion.h"
#include "mqtt_ha.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    logring_start();  // keep a copy of the log for the web page (/api/log)
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

    timekeep_init();
    webui_start();
    ota_check_start(hw_ok);  // after an update: keep it only once the hardware, Wi-Fi and web page work
    mqtt_ha_start();  // connects once Wi-Fi is up (settings: web page -> MQTT)

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
