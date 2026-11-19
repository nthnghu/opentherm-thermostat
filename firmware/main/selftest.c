#include "selftest.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_flash.h"
#include "esp_chip_info.h"
#include "esp_lvgl_port.h"
#include "esp_hosted.h"
#include "board.h"
#include "sht4x.h"
#include "camera.h"
#include "opentherm.h"

static const char *TAG = "selftest";

enum { T_MEM, T_TOUCH, T_SENSOR, T_AUDIO, T_WIFI, T_CAMERA, T_OT, T_COUNT };

static selftest_item_t s_items[T_COUNT] = {
    [T_MEM] = {"Memory (PSRAM, flash)", true},
    [T_TOUCH] = {"Touch (GT911)", true},
    [T_SENSOR] = {"Room sensor (SHT40)", true},
    [T_AUDIO] = {"Audio (ES8311)", false},
    [T_WIFI] = {"Wi-Fi chip (ESP32-C6)", false},
    [T_CAMERA] = {"Camera (OV02C10)", false},
    [T_OT] = {"OpenTherm shield", false},
};

static lv_obj_t *s_rows[T_COUNT];
static lv_obj_t *s_title;
static SemaphoreHandle_t s_continue;

static void set(int i, selftest_result_t r, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set(int i, selftest_result_t r, const char *fmt, ...)
{
    s_items[i].result = r;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_items[i].detail, sizeof(s_items[i].detail), fmt, ap);
    va_end(ap);
    static const char *icon[] = {"...", LV_SYMBOL_OK, LV_SYMBOL_WARNING, LV_SYMBOL_CLOSE};
    static const uint32_t color[] = {0x90A4AE, 0x81C784, 0xFFB74D, 0xE57373};
    ESP_LOGI(TAG, "%-24s %s %s", s_items[i].name, r == ST_PASS ? "PASS" : r == ST_WARN ? "WARN" : "FAIL",
             s_items[i].detail);
    lvgl_port_lock(0);
    lv_label_set_text_fmt(s_rows[i], "#%06lX %s#  %s\n    #90A4AE %s#", (unsigned long)color[r], icon[r], s_items[i].name,
                          s_items[i].detail);
    lvgl_port_unlock();
}

static void build_splash(void)
{
    lvgl_port_lock(0);
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_pad_all(scr, 24, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(scr, 14, 0);
    s_title = lv_label_create(scr);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_28, 0);
    lv_label_set_text(s_title, "Hardware check");
    for (int i = 0; i < T_COUNT; i++) {
        s_rows[i] = lv_label_create(scr);
        lv_label_set_recolor(s_rows[i], true);
        lv_obj_set_width(s_rows[i], 432);
        lv_label_set_long_mode(s_rows[i], LV_LABEL_LONG_WRAP);
        lv_label_set_text_fmt(s_rows[i], "#90A4AE ...#  %s", s_items[i].name);
    }
    lv_screen_load(scr);
    lvgl_port_unlock();
}

static void continue_cb(lv_event_t *e)
{
    xSemaphoreGive(s_continue);
}

static void test_memory(void)
{
    size_t psram = esp_psram_get_size();
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    set(T_MEM, psram >= 16 * 1024 * 1024 ? ST_PASS : ST_FAIL, "PSRAM %u MB, flash %lu MB", (unsigned)(psram >> 20),
        (unsigned long)(flash >> 20));
}

static void test_touch(void)
{
    bool probe = i2c_master_probe(board_i2c_bus(), 0x5D, 50) == ESP_OK || i2c_master_probe(board_i2c_bus(), 0x14, 50) == ESP_OK;
    if (board_touch_ok() && probe) {
        set(T_TOUCH, ST_PASS, "answering on I2C");
    } else if (!board_display_ok()) {
        set(T_TOUCH, ST_FAIL, "not started: the display failed");
    } else {
        set(T_TOUCH, ST_FAIL, probe ? "found, but the driver failed" : "not found on I2C (0x5D/0x14)");
    }
}

static void test_sensor(void)
{
    if (i2c_master_probe(board_i2c_bus(), 0x44, 50) != ESP_OK) {
        set(T_SENSOR, ST_FAIL, "not found on I2C 0x44 - check the CN3 cable (wires 3-4 swapped)");
        return;
    }
    uint32_t serial = 0;
    float t, h;
    esp_err_t err = sht4x_init(board_i2c_bus());
    if (err == ESP_OK) {
        err = sht4x_read_serial(&serial);
    }
    if (err == ESP_OK) {
        err = sht4x_measure(&t, &h);
    }
    if (err != ESP_OK) {
        set(T_SENSOR, ST_FAIL, "read error: %s", esp_err_to_name(err));
    } else if (t < 0 || t > 50) {
        set(T_SENSOR, ST_FAIL, "implausible reading %.1f C", t);
    } else {
        set(T_SENSOR, ST_PASS, "%.1f C, %.0f %%RH (serial %08lX)", t, h, (unsigned long)serial);
    }
}

static void test_audio(void)
{
    if (i2c_master_probe(board_i2c_bus(), 0x18, 50) != ESP_OK) {
        set(T_AUDIO, ST_WARN, "codec not found on I2C 0x18");
        return;
    }
    esp_err_t err = board_audio_init();
    set(T_AUDIO, err == ESP_OK ? ST_PASS : ST_WARN, err == ESP_OK ? "codec ready" : "codec init failed");
}

static void test_wifi(void)
{
    esp_err_t err = board_wifi_start();
    if (err != ESP_OK) {
        set(T_WIFI, ST_WARN, "%s", board_wifi_status());
        return;
    }
    esp_hosted_coprocessor_fwver_t v = {0};
    if (esp_hosted_get_coprocessor_fwversion(&v) == ESP_OK) {
        bool match = v.major1 == ESP_HOSTED_VERSION_MAJOR_1;
        set(T_WIFI, match ? ST_PASS : ST_WARN, "C6 firmware %lu.%lu.%lu%s", (unsigned long)v.major1,
            (unsigned long)v.minor1, (unsigned long)v.patch1, match ? "" : " (host expects 3.x: update in Setup)");
    } else {
        set(T_WIFI, ST_PASS, "link up (firmware version unknown)");
    }
}

static void test_camera(void)
{
    if (i2c_master_probe(board_i2c_bus(), 0x36, 50) != ESP_OK) {
        set(T_CAMERA, ST_WARN, "not found on I2C 0x36 - HA token scan unavailable");
        return;
    }
    int luma = -1;
    esp_err_t err = camera_selftest(&luma);
    if (err == ESP_OK) {
        set(T_CAMERA, ST_PASS, "frames OK, brightness %d", luma);
    } else {
        set(T_CAMERA, ST_WARN, "no frames (%s)", esp_err_to_name(err));
    }
}

static void test_opentherm(void)
{
    esp_err_t err = ot_init();
    if (err != ESP_OK) {
        set(T_OT, ST_WARN, "GPIO setup failed");
        return;
    }
    set(T_OT, ST_WARN, "input line %s; boiler not connected (simulation available)", ot_in_level() ? "high" : "low");
}

bool selftest_run(void)
{
    build_splash();
    test_memory();
    test_touch();
    test_sensor();
    test_audio();
    test_opentherm();
    test_camera();
    test_wifi();

    if (selftest_required_ok()) {
        lvgl_port_lock(0);
        lv_label_set_text(s_title, LV_SYMBOL_OK " Hardware check passed");
        lvgl_port_unlock();
        vTaskDelay(pdMS_TO_TICKS(1200));
        return true;
    }
    if (!board_display_ok()) {
        return false;  // no screen: nobody can press "Continue anyway", so the web page and MQTT start at once
    }
    // A required part failed: wait for "Continue anyway" (or 60 s, so the thermostat UI still comes up).
    s_continue = xSemaphoreCreateBinary();
    lvgl_port_lock(0);
    lv_label_set_text(s_title, "#E57373 " LV_SYMBOL_WARNING " A required part failed#");
    lv_label_set_recolor(s_title, true);
    lv_obj_t *b = lv_button_create(lv_obj_get_parent(s_title));
    lv_obj_set_size(b, 432, 70);
    lv_obj_set_style_bg_color(b, lv_color_hex(0xC62828), 0);
    lv_obj_add_event_cb(b, continue_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, "Continue anyway");
    lv_obj_center(l);
    lvgl_port_unlock();
    xSemaphoreTake(s_continue, pdMS_TO_TICKS(60000));
    return false;
}

int selftest_items(const selftest_item_t **items)
{
    *items = s_items;
    return T_COUNT;
}

bool selftest_required_ok(void)
{
    for (int i = 0; i < T_COUNT; i++) {
        if (s_items[i].required && s_items[i].result != ST_PASS) {
            return false;
        }
    }
    return true;
}
