#include "board.h"

#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"
#include "esp_codec_dev_defaults.h"
#include "esp_wifi.h"
#include "esp_hosted.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "driver/ledc.h"
#include "settings.h"
#include "debug.h"
#include "nvs.h"
#include "esp_netif_net_stack.h"
#include "lwip/dhcp.h"
#include "esp_system.h"

static const char *TAG = "board";

static i2c_master_bus_handle_t s_i2c;
static esp_codec_dev_handle_t s_codec;
static bool s_touch_ok;
static esp_lcd_panel_handle_t s_panel;

/* ---------------------------------------------------------------- display */

// ST7701S init table for the JC4880P443 (from ESPHome's mipi_dsi "JC4880P443" model).
typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t data[16];
} lcd_cmd_t;

static const lcd_cmd_t s_lcd_init[] = {
    {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x13}},
    {0xEF, 1, {0x08}},
    {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x10}},
    {0xC0, 2, {0x63, 0x00}},
    {0xC1, 2, {0x0D, 0x02}},
    {0xC2, 2, {0x10, 0x08}},
    {0xCC, 1, {0x10}},
    {0xB0, 16, {0x80, 0x09, 0x53, 0x0C, 0xD0, 0x07, 0x0C, 0x09, 0x09, 0x28, 0x06, 0xD4, 0x13, 0x69, 0x2B, 0x71}},
    {0xB1, 16, {0x80, 0x94, 0x5A, 0x10, 0xD3, 0x06, 0x0A, 0x08, 0x08, 0x25, 0x03, 0xD3, 0x12, 0x66, 0x6A, 0x0D}},
    {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x11}},
    {0xB0, 1, {0x5D}},
    {0xB1, 1, {0x58}},
    {0xB2, 1, {0x87}},
    {0xB3, 1, {0x80}},
    {0xB5, 1, {0x4E}},
    {0xB7, 1, {0x85}},
    {0xB8, 1, {0x21}},
    {0xB9, 2, {0x10, 0x1F}},
    {0xBB, 1, {0x03}},
    {0xBC, 1, {0x00}},
    {0xC1, 1, {0x78}},
    {0xC2, 1, {0x78}},
    {0xD0, 1, {0x88}},
    {0xE0, 3, {0x00, 0x3A, 0x02}},
    {0xE1, 11, {0x04, 0xA0, 0x00, 0xA0, 0x05, 0xA0, 0x00, 0xA0, 0x00, 0x40, 0x40}},
    {0xE2, 13, {0x30, 0x00, 0x40, 0x40, 0x32, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00}},
    {0xE3, 4, {0x00, 0x00, 0x33, 0x33}},
    {0xE4, 2, {0x44, 0x44}},
    {0xE5, 16, {0x09, 0x2E, 0xA0, 0xA0, 0x0B, 0x30, 0xA0, 0xA0, 0x05, 0x2A, 0xA0, 0xA0, 0x07, 0x2C, 0xA0, 0xA0}},
    {0xE6, 4, {0x00, 0x00, 0x33, 0x33}},
    {0xE7, 2, {0x44, 0x44}},
    {0xE8, 16, {0x08, 0x2D, 0xA0, 0xA0, 0x0A, 0x2F, 0xA0, 0xA0, 0x04, 0x29, 0xA0, 0xA0, 0x06, 0x2B, 0xA0, 0xA0}},
    {0xEB, 7, {0x00, 0x00, 0x4E, 0x4E, 0x00, 0x00, 0x00}},
    {0xEC, 2, {0x08, 0x01}},
    {0xED, 16, {0xB0, 0x2B, 0x98, 0xA4, 0x56, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xF7, 0x65, 0x4A, 0x89, 0xB2, 0x0B}},
    {0xEF, 6, {0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}},
    {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x00}},
};

static esp_err_t lcd_send_init(esp_lcd_panel_io_handle_t io)
{
    for (size_t i = 0; i < sizeof(s_lcd_init) / sizeof(s_lcd_init[0]); i++) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, s_lcd_init[i].cmd, s_lcd_init[i].data, s_lcd_init[i].len),
                            TAG, "init cmd 0x%02X", s_lcd_init[i].cmd);
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x11, NULL, 0), TAG, "SLPOUT");  // sleep out
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x29, NULL, 0), TAG, "DISPON");  // display on
    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

static esp_err_t touch_init(lv_display_t *disp)
{
    // GT911 answers on 0x5D or 0x14 depending on its INT level at power-up; use whichever responds.
    uint8_t addr = 0;
    if (i2c_master_probe(s_i2c, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, 100) == ESP_OK) {
        addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
    } else if (i2c_master_probe(s_i2c, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, 100) == ESP_OK) {
        addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
    } else {
        ESP_LOGE(TAG, "GT911 touch not found on I2C");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "GT911 touch at 0x%02X", addr);

    esp_lcd_panel_io_handle_t tio;
    esp_lcd_panel_io_i2c_config_t tio_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tio_cfg.dev_addr = addr;
    tio_cfg.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c, &tio_cfg, &tio), TAG, "touch io");

    esp_lcd_touch_io_gt911_config_t gt_cfg = {.dev_addr = addr};
    esp_lcd_touch_config_t tcfg = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,  // don't reset: that could change the I2C address
        .int_gpio_num = GPIO_NUM_NC,  // polled
        .driver_data = &gt_cfg,
    };
    esp_lcd_touch_handle_t tp;
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt911(tio, &tcfg, &tp), TAG, "gt911");

    const lvgl_port_touch_cfg_t lt = {.disp = disp, .handle = tp};
    return lvgl_port_add_touch(&lt) ? ESP_OK : ESP_FAIL;
}

esp_err_t board_i2c_init(void)
{
    if (s_i2c) {
        return ESP_OK;
    }
    // Shared I2C bus: room sensor (SHT40), touch (GT911), codec (ES8311)
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&i2c_cfg, &s_i2c);
}

// Backlight, DSI bus and the panel. Any failure here leaves the device without a screen but running.
static esp_err_t panel_init(esp_lcd_panel_io_handle_t *io_out, esp_lcd_panel_handle_t *panel_out)
{
    // Backlight: LEDC PWM, off until the panel shows something
    ledc_timer_config_t lt = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&lt), TAG, "ledc timer");
    ledc_channel_config_t lc = {
        .gpio_num = BOARD_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&lc), TAG, "ledc channel");

    // MIPI DSI PHY power: LDO channel 3 at 2.5 V
    esp_ldo_channel_handle_t ldo;
    esp_ldo_channel_config_t ldo_cfg = {.chan_id = 3, .voltage_mv = 2500};
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &ldo), TAG, "ldo");

    esp_lcd_dsi_bus_handle_t dsi;
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT,  // XTAL; the legacy default aborts on P4 rev v3.x
        .lane_bit_rate_mbps = 500,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &dsi), TAG, "dsi bus");

    esp_lcd_panel_io_handle_t io;
    esp_lcd_dbi_io_config_t dbi_cfg = {.virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8};
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi, &dbi_cfg, &io), TAG, "dbi io");

    // Hardware reset of the panel
    gpio_config_t rst = {.pin_bit_mask = 1ULL << BOARD_LCD_RST, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&rst);
    gpio_set_level(BOARD_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    ESP_RETURN_ON_ERROR(lcd_send_init(io), TAG, "lcd init");

    esp_lcd_panel_handle_t panel;
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 34,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .video_timing = {
            .h_size = BOARD_LCD_H_RES,
            .v_size = BOARD_LCD_V_RES,
            .hsync_pulse_width = 12,
            .hsync_back_porch = 42,
            .hsync_front_porch = 42,
            .vsync_pulse_width = 2,
            .vsync_back_porch = 8,
            .vsync_front_porch = 166,
        },
        .flags.use_dma2d = true,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_dpi(dsi, &dpi_cfg, &panel), TAG, "dpi panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "panel init");
    *io_out = io;
    *panel_out = panel;
    return ESP_OK;
}

static void headless_flush(lv_display_t *d, const lv_area_t *area, uint8_t *px)
{
    lv_display_flush_ready(d);
}

esp_err_t board_display_init(void)
{
    esp_err_t ierr = board_i2c_init();
    if (ierr != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus: %s (no room sensor, no touch)", esp_err_to_name(ierr));
    }
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    esp_err_t perr = panel_init(&io, &panel);

    // LVGL
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = 8192;
    port_cfg.task_affinity = 0;  // core 1 belongs to the thermostat
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");

    if (perr != ESP_OK) {
        // No screen: the thermostat, the web page and MQTT keep working. The UI is built on a display that
        // draws nowhere, so the rest of the firmware needs no special case.
        ESP_LOGE(TAG, "display failed (%s): running without a screen", esp_err_to_name(perr));
        size_t sz = BOARD_LCD_H_RES * 40 * 2;
        void *buf = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
        ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "headless buffer");
        lvgl_port_lock(0);
        lv_display_t *hd = lv_display_create(BOARD_LCD_H_RES, BOARD_LCD_V_RES);
        if (hd) {
            lv_display_set_color_format(hd, LV_COLOR_FORMAT_RGB565);
            lv_display_set_buffers(hd, buf, NULL, sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
            lv_display_set_flush_cb(hd, headless_flush);
        }
        lvgl_port_unlock();
        return hd ? ESP_OK : ESP_ERR_NO_MEM;
    }
    s_panel = panel;

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = BOARD_LCD_H_RES * 100,
        .double_buffer = true,
        .hres = BOARD_LCD_H_RES,
        .vres = BOARD_LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {.buff_spiram = true},
    };
    const lvgl_port_display_dsi_cfg_t dsi_disp_cfg = {.flags = {.avoid_tearing = false}};
    lv_display_t *disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_disp_cfg);
    if (!disp) {
        return ESP_FAIL;
    }

    board_backlight(80);

    s_touch_ok = touch_init(disp) == ESP_OK;
    if (!s_touch_ok) {
        ESP_LOGE(TAG, "touch init failed; continuing without touch");
    }
    return ESP_OK;
}

bool board_display_ok(void)
{
    return s_panel != NULL;
}

void board_backlight(int pct)
{
    pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);  // 0 = screen off
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, pct * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

/* ------------------------------------------------------------------ audio */

esp_err_t board_audio_init(void)
{
    i2s_chan_handle_t tx = NULL, rx = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK,
            .bclk = BOARD_I2S_BCLK,
            .ws = BOARD_I2S_WS,
            .dout = BOARD_I2S_DOUT,
            .din = BOARD_I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "i2s rx");

    audio_codec_i2s_cfg_t i2s_if_cfg = {.port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_if_cfg);

    audio_codec_i2c_cfg_t i2c_if_cfg = {.port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c};
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_if_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (!data_if || !ctrl_if || !gpio_if) {
        return ESP_FAIL;
    }

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = BOARD_PA_EN,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    if (!codec_if) {
        ESP_LOGE(TAG, "ES8311 not found");
        return ESP_ERR_NOT_FOUND;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    if (!s_codec) {
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = AUDIO_SAMPLE_RATE,
        .channel = 1,
        .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(s_codec, &fs) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    esp_codec_dev_set_out_vol(s_codec, 80);
    esp_codec_dev_set_in_gain(s_codec, 30.0);
    ESP_LOGI(TAG, "ES8311 audio ready");
    return ESP_OK;
}

esp_codec_dev_handle_t board_audio(void)
{
    return s_codec;
}

/* ------------------------------------------------------------------- wifi */

static volatile bool s_wifi_up;
static volatile bool s_wifi_inited;
static volatile int64_t s_cp_ok_us;   // last time the Wi-Fi chip (C6) answered
static uint32_t s_wd_restarts;        // Wi-Fi watchdog restarts so far (NVS)
static void wifi_mon_task(void *arg);
static char s_wifi_status[96] = "Wi-Fi off";
static char s_ip[16];
// Reconnect is done by wifi_mon_task, never in the esp_timer or event-loop task: a call into a slow or stuck Wi-Fi
// chip would stall every esp_timer (the LVGL tick among them) and make the screen crawl.
static volatile int64_t s_reconnect_at_us;  // 0 = none due

static void reconnect_in(int ms)
{
    s_reconnect_at_us = esp_timer_get_time() + (int64_t)ms * 1000 + 1;
}

static void set_wifi_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_wifi_status, sizeof(s_wifi_status), fmt, ap);
    va_end(ap);
}

static const char *reason_text(int reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
        return "network not found";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_CONNECTION_FAIL:
        return "wrong password?";
    default:
        return "retrying";
    }
}

// New Wi-Fi credentials are on trial: when they give no connection within WIFI_TRIAL_S, the previous ones (which
// worked) are put back, so a typo on the web page cannot take the device off the network for good.
#define WIFI_TRIAL_S 60
static char s_prev_ssid[33], s_prev_pass[65];
static volatile int64_t s_trial_until_us;  // 0 = no trial running

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (settings_wifi_configured()) {
            set_wifi_status("Connecting to %s...", settings_get()->ssid);
            reconnect_in(0);
        } else {
            set_wifi_status("Wi-Fi not set up");
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "Wi-Fi disconnected, reason %d", d->reason);
        s_wifi_up = false;
        s_ip[0] = 0;
        if (settings_wifi_configured()) {
            set_wifi_status("%s: %s (%d)", settings_get()->ssid, reason_text(d->reason), d->reason);
            reconnect_in(3000);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "Got IP %s", s_ip);
        s_wifi_up = true;
        s_trial_until_us = 0;  // new credentials (if any were on trial) work
        set_wifi_status("%s, %s", settings_get()->ssid, s_ip);
    }
}

esp_err_t board_wifi_start(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    esp_netif_create_default_wifi_sta();


    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        set_wifi_status("Wi-Fi chip (ESP32-C6) error %s", esp_err_to_name(err));
        return err;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);  // credentials live in our own NVS namespace
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");

    if (settings_wifi_configured()) {
        wifi_config_t wc = {0};
        strlcpy((char *)wc.sta.ssid, settings_get()->ssid, sizeof(wc.sta.ssid));
        strlcpy((char *)wc.sta.password, settings_get()->pass, sizeof(wc.sta.password));
        esp_wifi_set_config(WIFI_IF_STA, &wc);
    }
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    nvs_handle_t h;
    if (nvs_open("board", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "wd_rst", &s_wd_restarts);
        nvs_close(h);
    }
    s_cp_ok_us = esp_timer_get_time();
    s_wifi_inited = true;
    xTaskCreatePinnedToCore(wifi_mon_task, "wifi_mon", 4096, NULL, 2, NULL, 0);
    return ESP_OK;
}

int board_wifi_scan(wifi_ap_record_t *out, int max)
{
    if (!s_wifi_inited) {
        return -1;
    }
    wifi_scan_config_t sc = {.show_hidden = false};
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        // A connection attempt in progress blocks scanning: pause it and retry once.
        ESP_LOGW(TAG, "scan failed (%s), retrying", esp_err_to_name(err));
        s_reconnect_at_us = 0;
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(300));
        err = esp_wifi_scan_start(&sc, true);
        reconnect_in(3000);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan failed: %s", esp_err_to_name(err));
        return -1;
    }
    uint16_t n = max;
    esp_wifi_scan_get_ap_records(&n, out);
    ESP_LOGI(TAG, "scan found %d networks", n);
    return n;
}

esp_err_t board_wifi_connect(const char *ssid, const char *pass)
{
    if (!s_wifi_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    settings_t *s = malloc(sizeof(settings_t));  // ~2.4 KB: too big for timer-task stacks
    if (!s) {
        return ESP_ERR_NO_MEM;
    }
    settings_copy(s);
    bool changed = strcmp(s->ssid, ssid) != 0 || strcmp(s->pass, pass) != 0;
    if (s_trial_until_us) {
        s_trial_until_us = 0;  // a second change during a trial: the fallback stays the last working network
    } else if (changed && s->ssid[0] && s_wifi_up) {
        strlcpy(s_prev_ssid, s->ssid, sizeof(s_prev_ssid));
        strlcpy(s_prev_pass, s->pass, sizeof(s_prev_pass));
    } else {
        s_prev_ssid[0] = 0;
    }
    strlcpy(s->ssid, ssid, sizeof(s->ssid));
    strlcpy(s->pass, pass, sizeof(s->pass));
    esp_err_t serr = settings_save(s);
    free(s);
    ESP_RETURN_ON_ERROR(serr, TAG, "save");

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    s_reconnect_at_us = 0;
    esp_wifi_disconnect();
    s_wifi_up = false;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "config");
    set_wifi_status("Connecting to %s...", ssid);
    if (s_prev_ssid[0] && changed) {
        s_trial_until_us = esp_timer_get_time() + WIFI_TRIAL_S * 1000000LL;
    }
    return esp_wifi_connect();
}

bool board_wifi_connected(void)
{
    return s_wifi_up;
}

const char *board_wifi_status(void)
{
    return s_wifi_status;
}

const char *board_wifi_ip(void)
{
    return s_ip;
}

i2c_master_bus_handle_t board_i2c_bus(void)
{
    return s_i2c;
}

// Everything about the Wi-Fi link that needs the C6 (an RPC over SDIO, up to 5 s when the C6 hangs) is asked
// here, in one background task, and cached. The UI and MQTT only read the cache, so a stuck Wi-Fi chip can
// never freeze the screen. The same task is the Wi-Fi watchdog: when the C6 has not answered any request for
// settings wifi_wd_min minutes, the device restarts (the C6 is reset at start-up); 0 = never.
static volatile int s_rssi;

// Hang record (NVS "board"/"wd_log", newest first): when the C6 last answered, to look for a pattern.
#define WD_LOG_N 8
typedef struct {
    int64_t epoch;      // wall clock of the last answer (0 = clock not set)
    uint32_t uptime_s;  // uptime at the last answer
    int8_t rssi;        // last signal strength seen
} wd_event_t;

static void wd_log_add(int64_t epoch, uint32_t uptime_s, int rssi)
{
    wd_event_t ev[WD_LOG_N] = {0};
    nvs_handle_t h;
    if (nvs_open("board", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(ev);
    nvs_get_blob(h, "wd_log", ev, &len);
    memmove(&ev[1], &ev[0], sizeof(ev[0]) * (WD_LOG_N - 1));
    ev[0] = (wd_event_t){epoch, uptime_s, (int8_t)rssi};
    nvs_set_blob(h, "wd_log", ev, sizeof(ev));
    nvs_set_u32(h, "wd_rst", s_wd_restarts + 1);
    nvs_commit(h);
    nvs_close(h);
    s_wd_restarts++;  // the count of hangs handled (chip reset, or a device restart when that did not help)
}

void board_wifi_hang_log(char *out, size_t size)
{
    wd_event_t ev[WD_LOG_N] = {0};
    size_t len = sizeof(ev), n = 0;
    out[0] = 0;
    nvs_handle_t h;
    if (nvs_open("board", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_blob(h, "wd_log", ev, &len);
        nvs_close(h);
    }
    for (int i = 0; i < WD_LOG_N && n < size; i++) {
        if (!ev[i].uptime_s) {
            continue;
        }
        char when[24] = "clock not set";
        if (ev[i].epoch) {
            time_t t = (time_t)ev[i].epoch;
            struct tm lt;
            localtime_r(&t, &lt);
            strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &lt);
        }
        n += (size_t)snprintf(out + n, size - n, "%s%s, %lu min after start, signal %d", n ? "; " : "", when,
                              (unsigned long)(ev[i].uptime_s / 60), ev[i].rssi);
    }
    if (!out[0]) {
        snprintf(out, size, "none");
    }
}

// Wi-Fi chip recovery without restarting the thermostat: take the Wi-Fi driver and the link to the C6 down,
// bring the link up again (that resets the C6 through its reset pin), then start Wi-Fi with the saved network.
// Calls into a hung chip time out after 5 s each, so this can take half a minute; it runs in wifi_mon_task only.
#define CHIP_RECOVER_WAIT_S 90  // still silent this long after the chip reset: restart the device
static volatile bool s_chip_reset_req;
static uint32_t s_chip_resets;

static void wifi_chip_recover(void)
{
    int64_t t0 = esp_timer_get_time();
    s_wifi_up = false;
    s_ip[0] = 0;
    s_reconnect_at_us = 0;
    set_wifi_status("Restarting the Wi-Fi chip...");
    esp_err_t e1 = esp_wifi_stop();
    esp_err_t e2 = esp_wifi_deinit();
    int e3 = esp_hosted_deinit();
    vTaskDelay(pdMS_TO_TICKS(300));
    int e4 = esp_hosted_init();
    int e5 = esp_hosted_connect_to_slave();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t e6 = esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    if (settings_wifi_configured()) {
        wifi_config_t wc = {0};
        settings_t *c = malloc(sizeof(settings_t));
        if (c) {
            settings_copy(c);
            strlcpy((char *)wc.sta.ssid, c->ssid, sizeof(wc.sta.ssid));
            strlcpy((char *)wc.sta.password, c->pass, sizeof(wc.sta.password));
            free(c);
            esp_wifi_set_config(WIFI_IF_STA, &wc);
        }
    }
    esp_err_t e7 = esp_wifi_start();  // WIFI_EVENT_STA_START then reconnects
    s_chip_resets++;
    ESP_LOGW(TAG, "Wi-Fi chip reset #%lu in %lld ms: stop %s, deinit %s, link down %d, link up %d, connect %d, init %s, start %s",
             (unsigned long)s_chip_resets, (esp_timer_get_time() - t0) / 1000, esp_err_to_name(e1), esp_err_to_name(e2), e3,
             e4, e5, esp_err_to_name(e6), esp_err_to_name(e7));
}

void board_wifi_chip_reset(void)
{
    s_chip_reset_req = true;
}

uint32_t board_wifi_chip_resets(void)
{
    return s_chip_resets;
}

static void wifi_mon_task(void *arg)
{
    int64_t last_rssi_us = 0, ok_epoch = 0, next_check_us = 0, next_heap_us = 60000000, recover_at_us = 0;
    bool lease_logged = false;
    int ok_rssi = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (!s_wifi_inited) {
            continue;
        }
        int64_t now = esp_timer_get_time();
        int64_t rc = s_reconnect_at_us;
        if (rc && now >= rc) {
            s_reconnect_at_us = 0;
            if (settings_wifi_configured() && !s_wifi_up) {
                esp_wifi_connect();
            }
            now = esp_timer_get_time();
        }
        if (s_trial_until_us && now > s_trial_until_us && !s_wifi_up) {
            ESP_LOGW(TAG, "no connection with the new Wi-Fi settings in %d s: back to %s", WIFI_TRIAL_S, s_prev_ssid);
            char ssid[33], pass[65];
            strlcpy(ssid, s_prev_ssid, sizeof(ssid));
            strlcpy(pass, s_prev_pass, sizeof(pass));
            s_prev_ssid[0] = 0;  // no trial for the way back
            s_trial_until_us = 0;
            board_wifi_connect(ssid, pass);
            set_wifi_status("New network failed; back on %s", ssid);
            now = esp_timer_get_time();
        }
        if (now < next_check_us) {
            continue;
        }
        next_check_us = now + 5000000;
        if (s_wifi_up && !lease_logged) {  // look for timing patterns (hangs seen ~42 min after start)
            esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            struct netif *ln = nif ? esp_netif_get_netif_impl(nif) : NULL;
            struct dhcp *dh = ln ? netif_dhcp_data(ln) : NULL;
            if (dh) {
                ESP_LOGI(TAG, "DHCP lease %lu s (renew after %lu s, rebind after %lu s), uptime %lld s",
                         (unsigned long)dh->offered_t0_lease, (unsigned long)dh->offered_t1_renew,
                         (unsigned long)dh->offered_t2_rebind, now / 1000000);
            }
            lease_logged = true;
        } else if (!s_wifi_up) {
            lease_logged = false;
        }
        if (now >= next_heap_us) {  // memory trend for the Wi-Fi hang hunt (the SDIO driver ran out of DMA memory)
            next_heap_us = now + 600000000LL;
            char hb[200];
            debug_heap_stats(hb, sizeof(hb));
            ESP_LOGI(TAG, "memory: %s", hb);
        }
        bool answered = false;
        if (s_wifi_up) {
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                answered = true;
                if (ap.rssi != 0) {
                    s_rssi = ap.rssi;
                    last_rssi_us = now;
                }
            }
        } else {
            wifi_mode_t m;
            answered = esp_wifi_get_mode(&m) == ESP_OK;
        }
        now = esp_timer_get_time();
        if (answered) {
            s_cp_ok_us = now;
            time_t t = time(NULL);
            ok_epoch = t > 1700000000 ? (int64_t)t : 0;
            ok_rssi = s_rssi;
        }
        if (!s_wifi_up || now - last_rssi_us > 30000000) {
            s_rssi = 0;
        }
        int64_t silent_s = (now - s_cp_ok_us) / 1000000;
        uint16_t wd = settings_get()->wifi_wd_min;
        if (silent_s >= 15 && silent_s < 21) {  // what runs while the chip is silent (lag hunt)
            char *ts = malloc(1024);
            if (ts) {
                debug_task_stats(ts, 1024);
                ESP_LOGW(TAG, "Wi-Fi chip silent %lld s; CPU: %s", silent_s, ts);
                debug_heap_stats(ts, 1024);
                ESP_LOGW(TAG, "memory: %s", ts);
                free(ts);
            }
        }
        if (silent_s >= 60 && silent_s % 60 < 6) {
            ESP_LOGW(TAG, "Wi-Fi chip not answering for %lld s (restart after %u min)", silent_s, wd);
        }
        if (answered) {
            recover_at_us = 0;
        }
        if (s_chip_reset_req || (wd && silent_s >= (int64_t)wd * 60 && !recover_at_us)) {
            // First the Wi-Fi chip alone: the thermostat keeps running and the boiler sees no gap.
            bool test = s_chip_reset_req;
            s_chip_reset_req = false;
            ESP_LOGE(TAG, "Wi-Fi chip %s: resetting the chip only", test ? "reset requested" : "silent");
            if (!test) {
                wd_log_add(ok_epoch, (uint32_t)(s_cp_ok_us / 1000000), ok_rssi);
            }
            wifi_chip_recover();
            recover_at_us = esp_timer_get_time();
            next_check_us = recover_at_us + 5000000;
            continue;
        }
        if (wd && recover_at_us && now - recover_at_us > CHIP_RECOVER_WAIT_S * 1000000LL && silent_s >= CHIP_RECOVER_WAIT_S) {
            // The chip reset did not bring it back: restart everything, as before.
            ESP_LOGE(TAG, "Wi-Fi chip still silent %d s after its reset: restarting the device", CHIP_RECOVER_WAIT_S);
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
    }
}

int board_wifi_rssi(void)
{
    return s_wifi_up ? s_rssi : 0;
}

int board_wifi_cp_silent_s(void)
{
    return s_wifi_inited ? (int)((esp_timer_get_time() - s_cp_ok_us) / 1000000) : 0;
}

uint32_t board_wifi_wd_restarts(void)
{
    return s_wd_restarts;
}

bool board_touch_ok(void)
{
    return s_touch_ok;
}

const uint16_t *board_framebuffer(void)
{
    void *fb = NULL;
    if (s_panel) {
        esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &fb);
    }
    return fb;
}
