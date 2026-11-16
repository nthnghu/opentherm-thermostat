#include "ota_check.h"
#include <stdio.h>
#include <errno.h>
#include "lwip/sockets.h"
#include "webui.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board.h"

static const char *TAG = "ota";
static volatile ota_st_t s_state = OTA_ST_VALID;
static char s_text[96] = "accepted";

// The web server works when it has handled a request since the restart (tools/ota.sh and the web page poll it),
// or when a TCP connection to its port succeeds over loopback.
static bool web_answers(void)
{
    if (webui_requests() > 0) {
        return true;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    struct timeval tv = {.tv_sec = 3};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(80)};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0;
    if (!ok) {
        ESP_LOGW(TAG, "trial: loopback connect to the web server failed (errno %d)", errno);
    }
    close(fd);
    return ok;
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
