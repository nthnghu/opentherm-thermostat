#include "ota_check.h"
#include <stdio.h>
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board.h"

static const char *TAG = "ota";
static volatile ota_st_t s_state = OTA_ST_VALID;
static char s_text[96] = "accepted";

// Any HTTP answer (401 without a login is fine) proves the web server runs.
static bool web_answers(void)
{
    esp_http_client_config_t cfg = {.url = "http://127.0.0.1/api/status", .timeout_ms = 3000};
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) {
        return false;
    }
    esp_err_t err = esp_http_client_perform(h);
    int status = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    return err == ESP_OK && status > 0;
}

static void trial_task(void *arg)
{
    int64_t t0 = esp_timer_get_time();
    for (;;) {
        int left = OTA_TRIAL_S - (int)((esp_timer_get_time() - t0) / 1000000);
        bool wifi = board_wifi_connected();
        bool web = wifi && web_answers();
        if (wifi && web) {
            esp_ota_mark_app_valid_cancel_rollback();
            s_state = OTA_ST_VALID;
            snprintf(s_text, sizeof(s_text), "accepted (Wi-Fi and web page checked after the update)");
            ESP_LOGI(TAG, "new firmware: Wi-Fi and web server OK, keeping it");
            vTaskDelete(NULL);
        }
        if (left <= 0) {
            ESP_LOGE(TAG, "new firmware: %s after %d s, rolling back", wifi ? "web server not answering" : "no Wi-Fi",
                     OTA_TRIAL_S);
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
        snprintf(s_text, sizeof(s_text), "on trial: waiting for %s (%d s left)", wifi ? "the web server" : "Wi-Fi",
                 left);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void ota_check_start(bool hw_ok)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK) {
        s_state = OTA_ST_FACTORY;
        snprintf(s_text, sizeof(s_text), "flashed over USB");
        return;
    }
    if (st != ESP_OTA_IMG_PENDING_VERIFY) {
        if (st != ESP_OTA_IMG_VALID) {
            s_state = OTA_ST_FACTORY;
            snprintf(s_text, sizeof(s_text), "flashed over USB");
        }
        return;
    }
    if (!hw_ok) {
        ESP_LOGE(TAG, "new firmware failed the hardware check: rolling back");
        esp_ota_mark_app_invalid_rollback_and_reboot();
        return;
    }
    s_state = OTA_ST_TRIAL;
    snprintf(s_text, sizeof(s_text), "on trial: waiting for Wi-Fi");
    xTaskCreatePinnedToCore(trial_task, "ota_trial", 4096, NULL, 2, NULL, 0);
}

ota_st_t ota_check_state(void)
{
    return s_state;
}

const char *ota_check_text(void)
{
    return s_text;
}
