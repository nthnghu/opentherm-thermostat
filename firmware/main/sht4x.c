#include "sht4x.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define SHT4X_ADDR        0x44
#define CMD_MEASURE_HIGH  0xFD
#define CMD_SERIAL        0x89

static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_lock;  // a measurement is command + wait + read: keep callers apart

static uint8_t crc8(const uint8_t *d, int n)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1;
        }
    }
    return crc;
}

esp_err_t sht4x_init(i2c_master_bus_handle_t bus)
{
    if (s_dev) {
        return ESP_OK;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SHT4X_ADDR,
        .scl_speed_hz = 100000,
    };
    return i2c_master_bus_add_device(bus, &dc, &s_dev);
}

// Sends cmd, waits, reads two CRC-protected words.
static esp_err_t cmd_read_locked(uint8_t cmd, int wait_ms, uint16_t *w0, uint16_t *w1)
{
    esp_err_t err = i2c_master_transmit(s_dev, &cmd, 1, 50);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(wait_ms));
    uint8_t b[6];
    err = i2c_master_receive(s_dev, b, sizeof(b), 50);
    if (err != ESP_OK) {
        return err;
    }
    if (crc8(b, 2) != b[2] || crc8(b + 3, 2) != b[5]) {
        return ESP_ERR_INVALID_CRC;
    }
    *w0 = (b[0] << 8) | b[1];
    *w1 = (b[3] << 8) | b[4];
    return ESP_OK;
}

static esp_err_t cmd_read(uint8_t cmd, int wait_ms, uint16_t *w0, uint16_t *w1)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = cmd_read_locked(cmd, wait_ms, w0, w1);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t sht4x_read_serial(uint32_t *serial)
{
    uint16_t a, b;
    esp_err_t err = cmd_read(CMD_SERIAL, 2, &a, &b);
    if (err == ESP_OK) {
        *serial = ((uint32_t)a << 16) | b;
    }
    return err;
}

esp_err_t sht4x_measure(float *temp_c, float *rh)
{
    uint16_t t, h;
    esp_err_t err = cmd_read(CMD_MEASURE_HIGH, 10, &t, &h);
    if (err != ESP_OK) {
        return err;
    }
    *temp_c = -45.0f + 175.0f * t / 65535.0f;
    float r = -6.0f + 125.0f * h / 65535.0f;
    *rh = r < 0 ? 0 : (r > 100 ? 100 : r);
    return ESP_OK;
}
