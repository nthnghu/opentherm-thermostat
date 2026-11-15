#pragma once
// Device settings in NVS (namespace "cfg"). Set on the device (Setup) or the web page.
// Thermostat parameters live in thermostat.c (namespace "thermo").
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define SETTINGS_HA_MAX_ENTITIES 24

typedef struct {
    // Wi-Fi
    char ssid[33];
    char pass[65];
    // Home Assistant
    char ha_url[128];
    char ha_token[320];
    char ha_entities[SETTINGS_HA_MAX_ENTITIES * 64];  // comma-separated entity_ids for the HA app
    // Clock
    bool ntp_on;
    char ntp_server[64];
    char tz[64];  // POSIX TZ string
    // Web access (HTTP Basic auth); empty hash = not set up, web page refuses access
    char web_user[33];
    char web_salt[33];  // hex
    char web_hash[65];  // hex SHA-256(salt || password)
    // Display
    uint8_t bright;       // %, normal
    uint8_t dim_level;    // %, after inactivity
    uint16_t dim_after_s; // 0 = never dim
    bool night_on;
    uint16_t night_start; // minutes after midnight
    uint16_t night_end;
    uint8_t night_level;  // %
    uint16_t home_after_s; // back to the thermostat after this much inactivity (0 = never)
    uint16_t off_after_s;  // screen off after this much inactivity (0 = never)
    bool keep_on;          // schedule: the screen does not turn off between keep_on_start and keep_on_end
    uint16_t keep_on_start; // minutes after midnight
    uint16_t keep_on_end;
    bool motion_wake;      // camera motion turns the screen on (and keeps it on)
    uint8_t motion_sens;   // 0 = low, 1 = medium, 2 = high
    uint8_t theme_mode;    // 0 = auto (HA sun, else day window), 1 = light, 2 = dark
    uint16_t day_start;    // light theme window when HA's sun is unknown (minutes after midnight)
    uint16_t day_end;
    // MQTT (Home Assistant discovery + telemetry for Telegraf; mqtt_ha.c)
    bool mqtt_on;
    char mqtt_uri[96];     // mqtt://host:1883
    char mqtt_user[64];
    char mqtt_pass[96];
    char mqtt_loc[32];     // topic sensors/<loc>/<dev>
    char mqtt_dev[32];
} settings_t;

void settings_load(void);
// Read-only view; fields can change when another task saves. Use settings_copy() in background tasks.
const settings_t *settings_get(void);
void settings_copy(settings_t *out);
esp_err_t settings_save(const settings_t *s);
bool settings_wifi_configured(void);

// Web credentials
void settings_set_web_password(settings_t *s, const char *user, const char *password);
bool settings_check_web_password(const char *user, const char *password);
bool settings_web_configured(void);
