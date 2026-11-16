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
#include "esp_core_dump.h"
#include "ota_check.h"
#include "motion.h"
#include "mqtt_ha.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    bool nvs_erased = false;
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_erased = true;
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    logring_start();  // keep a copy of the log for the web page (/api/log)
    if (nvs_erased || err != ESP_OK) {
        ESP_LOGE(TAG, "settings storage (NVS): %s%s", nvs_erased ? "was full or outdated and has been erased; " : "",
                 err == ESP_OK ? "all settings are back at their defaults" : esp_err_to_name(err));
    }
    settings_load();
    {
        // A crash dump from an earlier run (kept in flash until downloaded and cleared: web page, Diagnostics)
        esp_core_dump_summary_t *cs = malloc(sizeof(*cs));
        if (cs && esp_core_dump_image_check() == ESP_OK && esp_core_dump_get_summary(cs) == ESP_OK) {
            ESP_LOGE(TAG, "crash dump in flash: task \"%s\" at 0x%08lx (cause %lu, return 0x%08lx), firmware %.9s",
                     cs->exc_task, (unsigned long)cs->exc_pc, (unsigned long)cs->ex_info.mcause,
                     (unsigned long)cs->ex_info.ra, (const char *)cs->app_elf_sha256);
        }
        free(cs);
    }

    // Heating first: nothing below (display, self-test, Wi-Fi) can keep the thermostat from starting.
    if (board_i2c_init() != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus failed: no room sensor (the sensor-loss fallback applies)");
    }
    sht4x_init(board_i2c_bus());
    otbus_start();  // OpenTherm cadence from here on
    thermo_start();

    if (board_display_init() != ESP_OK) {
        // Not even the drawing library came up (out of memory): the rest of the firmware needs it. Heating keeps
        // running on the saved settings, without screen, Wi-Fi or web page.
        ESP_LOGE(TAG, "user interface could not start: heating only");
        return;
    }
    board_backlight(settings_get()->bright);

    lvgl_port_lock(0);
    ui_theme_init();  // dark until the day/night logic runs
    lvgl_port_unlock();

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
