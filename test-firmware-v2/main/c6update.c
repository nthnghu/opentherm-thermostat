// Updates the ESP32-C6 Wi-Fi coprocessor firmware (ESP-Hosted) from the "c6fw" flash partition,
// streamed over SDIO with the host-driven OTA RPCs. The image is written to the C6's spare OTA slot,
// so an interrupted transfer leaves the C6 on its current firmware.
//
// Partition layout: 16-byte header {"C6FW", u32 length, u32 crc32, u32 reserved} + the .bin.
// Pack and flash it with tools/flash_c6fw.sh.
#include "c6update.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_hosted.h"
#include "esp_hosted_ota.h"

static const char *TAG = "c6update";

#define CHUNK 1024  // below EH_HOST_CP_OTA_CHUNK_MAX

typedef struct {
    char magic[4];
    uint32_t len;
    uint32_t crc;
    uint32_t reserved;
} c6_hdr_t;

static const esp_partition_t *find_part(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "c6fw");
}

bool c6update_image_info(uint32_t *len)
{
    const esp_partition_t *p = find_part();
    c6_hdr_t h;
    if (!p || esp_partition_read(p, 0, &h, sizeof(h)) != ESP_OK || memcmp(h.magic, "C6FW", 4) != 0 ||
        h.len == 0 || h.len > p->size - sizeof(h)) {
        return false;
    }
    if (len) {
        *len = h.len;
    }
    return true;
}

esp_err_t c6update_run(c6update_progress_cb_t progress, void *user)
{
    const esp_partition_t *p = find_part();
    c6_hdr_t h;
    if (!p || !c6update_image_info(NULL)) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_partition_read(p, 0, &h, sizeof(h));
    uint8_t *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_INTERNAL);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    // 1. Verify the stored image before touching the C6.
    uint32_t crc = 0;
    for (uint32_t off = 0; off < h.len; off += CHUNK) {
        uint32_t n = h.len - off < CHUNK ? h.len - off : CHUNK;
        esp_partition_read(p, sizeof(h) + off, buf, n);
        crc = esp_rom_crc32_le(crc, buf, n);
    }
    if (crc != h.crc) {
        free(buf);
        ESP_LOGE(TAG, "stored image CRC mismatch");
        return ESP_ERR_INVALID_CRC;
    }

    // 2. Stream it.
    ESP_LOGW(TAG, "updating the C6 firmware: %lu bytes", (unsigned long)h.len);
    esp_err_t err = esp_hosted_slave_ota_begin();
    if (err != ESP_OK) {
        free(buf);
        ESP_LOGE(TAG, "OTA begin: %s", esp_err_to_name(err));
        return err;
    }
    for (uint32_t off = 0; off < h.len && err == ESP_OK; off += CHUNK) {
        uint32_t n = h.len - off < CHUNK ? h.len - off : CHUNK;
        esp_partition_read(p, sizeof(h) + off, buf, n);
        err = esp_hosted_slave_ota_write(buf, n);
        if (progress && (off / CHUNK) % 32 == 0) {
            progress((int)((uint64_t)off * 100 / h.len), user);
        }
    }
    free(buf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA write: %s", esp_err_to_name(err));
        esp_hosted_slave_ota_end();
        return err;
    }
    err = esp_hosted_slave_ota_end();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA end: %s", esp_err_to_name(err));
        return err;
    }
    if (progress) {
        progress(100, user);
    }

    // 3. Activate (CP firmware 2.6+ needs an explicit call; older ones switch on end) and restart both.
    esp_hosted_coprocessor_fwver_t v = {0};
    if (esp_hosted_get_coprocessor_fwversion(&v) == ESP_OK && (v.major1 > 2 || (v.major1 == 2 && v.minor1 > 5))) {
        err = esp_hosted_slave_ota_activate();
        ESP_LOGI(TAG, "activate: %s", esp_err_to_name(err));
    }
    ESP_LOGW(TAG, "C6 updated; restarting to resync");
    esp_hosted_deinit();
    vTaskDelay(pdMS_TO_TICKS(2000));  // let the C6 boot the new image before the host resets it
    esp_restart();
    return ESP_OK;
}
