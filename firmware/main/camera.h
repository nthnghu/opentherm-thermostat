#pragma once
// OV02C10 MIPI-CSI camera: RGB565 frames and QR decoding.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef void (*camera_frame_cb_t)(const uint16_t *rgb565, int w, int h, void *user);

// Thread-safe: start/stop/grab may be called from several tasks.
esp_err_t camera_init(void);   // once; needs board_display_init() first (I2C bus + MIPI LDO)
esp_err_t camera_start(void);  // start streaming
void camera_stop(void);
bool camera_streaming(void);
int camera_width(void);
int camera_height(void);
// Waits for the next frame and passes it to cb (the buffer is only valid inside cb).
bool camera_grab(camera_frame_cb_t cb, void *user);
// Boot test: grabs a few frames; returns ESP_OK when frames arrive.
esp_err_t camera_selftest(int *mean_luma);
