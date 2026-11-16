#pragma once
// Remote setup page on port 80 with HTTP Basic auth (credentials set on the device first).
// Settings are editable; the thermostat is shown read-only (no control over HTTP).
#include "esp_err.h"

esp_err_t webui_start(void);
#include <stdint.h>
uint32_t webui_requests(void);  // requests handled since start (any client, with or without login)
