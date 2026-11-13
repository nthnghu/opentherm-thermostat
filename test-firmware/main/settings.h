#pragma once
// Wi-Fi + Home Assistant settings, stored in NVS (set up on the device via the Setup screen).
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    char ssid[33];
    char pass[65];
    char ha_url[128];
    char ha_token[320];
} settings_t;

void settings_load(void);
const settings_t *settings_get(void);
esp_err_t settings_save(const settings_t *s);
bool settings_wifi_configured(void);
