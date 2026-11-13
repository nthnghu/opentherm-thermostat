#include "settings.h"
#include <string.h>
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "settings";
static settings_t s_cfg;

// Reads a string key; falls back to the build-time (menuconfig) value when the key is missing.
static void load_str(nvs_handle_t h, const char *key, char *out, size_t size, const char *fallback)
{
    size_t len = size;
    if (h == 0 || nvs_get_str(h, key, out, &len) != ESP_OK) {
        strlcpy(out, fallback, size);
    }
}

void settings_load(void)
{
    nvs_handle_t h = 0;
    if (nvs_open("cfg", NVS_READONLY, &h) != ESP_OK) {
        h = 0;
    }
    load_str(h, "ssid", s_cfg.ssid, sizeof(s_cfg.ssid), CONFIG_TF_WIFI_SSID);
    load_str(h, "pass", s_cfg.pass, sizeof(s_cfg.pass), CONFIG_TF_WIFI_PASSWORD);
    load_str(h, "ha_url", s_cfg.ha_url, sizeof(s_cfg.ha_url), CONFIG_TF_HA_URL);
    load_str(h, "ha_token", s_cfg.ha_token, sizeof(s_cfg.ha_token), CONFIG_TF_HA_TOKEN);
    if (h) {
        nvs_close(h);
    }
    // Strip a trailing slash so "<url>/api/states" is well-formed.
    size_t n = strlen(s_cfg.ha_url);
    if (n && s_cfg.ha_url[n - 1] == '/') {
        s_cfg.ha_url[n - 1] = 0;
    }
    ESP_LOGI(TAG, "Wi-Fi: %s, HA: %s, token %s", s_cfg.ssid[0] ? s_cfg.ssid : "(none)", s_cfg.ha_url,
             s_cfg.ha_token[0] ? "set" : "missing");
}

const settings_t *settings_get(void)
{
    return &s_cfg;
}

bool settings_wifi_configured(void)
{
    return s_cfg.ssid[0] != 0;
}

esp_err_t settings_save(const settings_t *s)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("cfg", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_str(h, "ssid", s->ssid);
    nvs_set_str(h, "pass", s->pass);
    nvs_set_str(h, "ha_url", s->ha_url);
    nvs_set_str(h, "ha_token", s->ha_token);
    err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        s_cfg = *s;
    }
    return err;
}
