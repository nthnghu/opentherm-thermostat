#pragma once
// Board support for the Guition JC4880P443C_I_W (ESP32-P4 + ESP32-C6).
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_codec_dev.h"
#include "esp_wifi_types.h"

#define BOARD_LCD_H_RES 480
#define BOARD_LCD_V_RES 800

// Pins (community pin map, see the build plan doc)
#define BOARD_I2C_SDA      7
#define BOARD_I2C_SCL      8
#define BOARD_LCD_RST      5
#define BOARD_LCD_BL       23
#define BOARD_I2S_MCLK     13
#define BOARD_I2S_BCLK     12
#define BOARD_I2S_WS       10
#define BOARD_I2S_DOUT     9
#define BOARD_I2S_DIN      48
#define BOARD_PA_EN        11

#define AUDIO_SAMPLE_RATE  16000

// I2C bus, display, touch and LVGL. Must be called first.
esp_err_t board_i2c_init(void);      // shared bus (room sensor, touch, codec); safe to call twice
esp_err_t board_display_init(void);  // a failed panel is not an error: the UI then runs on a display that draws nowhere
bool board_display_ok(void);         // a real screen is attached and working
esp_err_t board_test_nowifi_next(void);  // console "TEST nowifi": the next start can't reach Wi-Fi (T16)
esp_err_t board_test_nodisplay_next(void);  // test: the next start runs as if the display had failed (once)
// Backlight brightness 0 (off) .. 100 %.
void board_backlight(int pct);
bool board_touch_ok(void);
// The displayed RGB565 frame (480 x 800), for the USB debug screenshot.
const uint16_t *board_framebuffer(void);  // GT911 found and registered with LVGL

// ES8311 codec (speaker + mic), opened at AUDIO_SAMPLE_RATE, 16-bit mono.
esp_err_t board_audio_init(void);
esp_codec_dev_handle_t board_audio(void);

// Starts Wi-Fi (through the ESP32-C6) and connects to the saved network, reconnecting on its own.
esp_err_t board_wifi_start(void);
// Blocking scan (a few seconds); returns the number of records written, or -1.
int board_wifi_scan(wifi_ap_record_t *out, int max);
// Saves the network to NVS and connects to it.
esp_err_t board_wifi_connect(const char *ssid, const char *pass);
bool board_wifi_connected(void);
const char *board_wifi_status(void);
const char *board_wifi_ip(void);  // "" when not connected
int board_wifi_rssi(void);        // 0 when not connected; cached, never blocks
int board_wifi_cp_silent_s(void); // seconds since the Wi-Fi chip (C6) last answered (normally < 10)
void board_wifi_chip_reset(void);       // test: reset the Wi-Fi chip alone, as the watchdog does first
uint32_t board_wifi_chip_resets(void);  // chip resets since start
uint32_t board_wifi_wd_restarts(void);  // restarts by the Wi-Fi watchdog so far
void board_wifi_hang_log(char *out, size_t size);  // the last hangs, newest first ("none")
void board_wifi_drop_log(char *out, size_t size);  // network losses, a current one first, then newest first
uint32_t board_wifi_drops(void);                   // network losses so far (NVS)
void board_wifi_drop_log_clear(void);             // erase the drop log and its count

#include "driver/i2c_master.h"
// Shared I2C bus (GPIO7/8): GT911, ES8311, SHT40, camera SCCB.
i2c_master_bus_handle_t board_i2c_bus(void);
