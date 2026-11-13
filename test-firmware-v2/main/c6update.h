#pragma once
// ESP32-C6 (Wi-Fi coprocessor) firmware update from the "c6fw" partition.
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef void (*c6update_progress_cb_t)(int percent, void *user);

// True when a valid image is stored; *len = its size.
bool c6update_image_info(uint32_t *len);
// Streams the image to the C6 and restarts the device on success (does not return then).
esp_err_t c6update_run(c6update_progress_cb_t progress, void *user);
