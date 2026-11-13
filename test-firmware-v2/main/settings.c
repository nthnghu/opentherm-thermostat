#include "settings.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/sha256.h"

static const char *TAG = "settings";
static settings_t s_cfg;
static SemaphoreHandle_t s_lock;

static void load_str(nvs_handle_t h, const char *key, char *out, size_t size, const char *fallback)
{
    size_t len = size;
    if (h == 0 || nvs_get_str(h, key, out, &len) != ESP_OK) {
        strlcpy(out, fallback, size);
    }
}

static void load_u8(nvs_handle_t h, const char *key, uint8_t *out, uint8_t def)
{
    if (h == 0 || nvs_get_u8(h, key, out) != ESP_OK) {
        *out = def;
    }
}

static void load_u16(nvs_handle_t h, const char *key, uint16_t *out, uint16_t def)
{
    if (h == 0 || nvs_get_u16(h, key, out) != ESP_OK) {
        *out = def;
    }
}

void settings_load(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    nvs_handle_t h = 0;
    if (nvs_open("cfg", NVS_READONLY, &h) != ESP_OK) {
        h = 0;
    }
    settings_t *c = &s_cfg;
    load_str(h, "ssid", c->ssid, sizeof(c->ssid), CONFIG_TF_WIFI_SSID);
    load_str(h, "pass", c->pass, sizeof(c->pass), CONFIG_TF_WIFI_PASSWORD);
    load_str(h, "ha_url", c->ha_url, sizeof(c->ha_url), CONFIG_TF_HA_URL);
    load_str(h, "ha_token", c->ha_token, sizeof(c->ha_token), CONFIG_TF_HA_TOKEN);
    load_str(h, "ha_ents", c->ha_entities, sizeof(c->ha_entities), "");
    uint8_t ntp;
    load_u8(h, "ntp_on", &ntp, 1);
    c->ntp_on = ntp;
    load_str(h, "ntp_srv", c->ntp_server, sizeof(c->ntp_server), "pool.ntp.org");
    load_str(h, "tz", c->tz, sizeof(c->tz), "CET-1CEST,M3.5.0,M10.5.0/3");  // Europe/Budapest
    load_str(h, "web_user", c->web_user, sizeof(c->web_user), "admin");
    load_str(h, "web_salt", c->web_salt, sizeof(c->web_salt), "");
    load_str(h, "web_hash", c->web_hash, sizeof(c->web_hash), "");
    load_u8(h, "bright", &c->bright, 80);
    load_u8(h, "dim_lvl", &c->dim_level, 15);
    load_u16(h, "dim_s", &c->dim_after_s, 60);
    uint8_t night;
    load_u8(h, "night_on", &night, 0);
    c->night_on = night;
    load_u16(h, "night_st", &c->night_start, 22 * 60);
    load_u16(h, "night_en", &c->night_end, 6 * 60 + 30);
    load_u8(h, "night_lvl", &c->night_level, 5);
    load_u16(h, "home_s", &c->home_after_s, 60);
    load_u16(h, "off_s", &c->off_after_s, 0);
    uint8_t b8;
    load_u8(h, "keep_on", &b8, 0);
    c->keep_on = b8;
    load_u16(h, "keep_st", &c->keep_on_start, 7 * 60);
    load_u16(h, "keep_en", &c->keep_on_end, 22 * 60);
    load_u8(h, "motion", &b8, 0);
    c->motion_wake = b8;
    load_u8(h, "mot_sens", &c->motion_sens, 1);
    if (h) {
        nvs_close(h);
    }
    size_t n = strlen(c->ha_url);
    if (n && c->ha_url[n - 1] == '/') {
        c->ha_url[n - 1] = 0;
    }
    ESP_LOGI(TAG, "Wi-Fi: %s, HA: %s, token %s, web login %s", c->ssid[0] ? c->ssid : "(none)", c->ha_url,
             c->ha_token[0] ? "set" : "missing", c->web_hash[0] ? "set" : "not set");
}

const settings_t *settings_get(void)
{
    return &s_cfg;
}

void settings_copy(settings_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
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
    // Count failed writes: a partial save must not look like success.
    int bad = 0;
#define CHK(x) bad += (x) != ESP_OK
    CHK(nvs_set_str(h, "ssid", s->ssid));
    CHK(nvs_set_str(h, "pass", s->pass));
    CHK(nvs_set_str(h, "ha_url", s->ha_url));
    CHK(nvs_set_str(h, "ha_token", s->ha_token));
    CHK(nvs_set_str(h, "ha_ents", s->ha_entities));
    CHK(nvs_set_u8(h, "ntp_on", s->ntp_on));
    CHK(nvs_set_str(h, "ntp_srv", s->ntp_server));
    CHK(nvs_set_str(h, "tz", s->tz));
    CHK(nvs_set_str(h, "web_user", s->web_user));
    CHK(nvs_set_str(h, "web_salt", s->web_salt));
    CHK(nvs_set_str(h, "web_hash", s->web_hash));
    CHK(nvs_set_u8(h, "bright", s->bright));
    CHK(nvs_set_u8(h, "dim_lvl", s->dim_level));
    CHK(nvs_set_u16(h, "dim_s", s->dim_after_s));
    CHK(nvs_set_u8(h, "night_on", s->night_on));
    CHK(nvs_set_u16(h, "night_st", s->night_start));
    CHK(nvs_set_u16(h, "night_en", s->night_end));
    CHK(nvs_set_u8(h, "night_lvl", s->night_level));
    CHK(nvs_set_u16(h, "home_s", s->home_after_s));
    CHK(nvs_set_u16(h, "off_s", s->off_after_s));
    CHK(nvs_set_u8(h, "keep_on", s->keep_on));
    CHK(nvs_set_u16(h, "keep_st", s->keep_on_start));
    CHK(nvs_set_u16(h, "keep_en", s->keep_on_end));
    CHK(nvs_set_u8(h, "motion", s->motion_wake));
    CHK(nvs_set_u8(h, "mot_sens", s->motion_sens));
#undef CHK
    err = nvs_commit(h);
    nvs_close(h);
    if (bad) {
        ESP_LOGE(TAG, "%d settings could not be written to NVS", bad);
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "saved (token %s)", s->ha_token[0] ? "set" : "empty");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_cfg = *s;
        xSemaphoreGive(s_lock);
    }
    return err;
}

static void to_hex(const uint8_t *in, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) {
        sprintf(out + i * 2, "%02x", in[i]);
    }
}

static void hash_pw(const char *salt, const char *pw, char out[65])
{
    uint8_t d[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, (const uint8_t *)salt, strlen(salt));
    mbedtls_sha256_update(&ctx, (const uint8_t *)pw, strlen(pw));
    mbedtls_sha256_finish(&ctx, d);
    mbedtls_sha256_free(&ctx);
    to_hex(d, 32, out);
}

void settings_set_web_password(settings_t *s, const char *user, const char *password)
{
    uint8_t salt[16];
    esp_fill_random(salt, sizeof(salt));
    to_hex(salt, sizeof(salt), s->web_salt);
    strlcpy(s->web_user, user, sizeof(s->web_user));
    hash_pw(s->web_salt, password, s->web_hash);
}

bool settings_check_web_password(const char *user, const char *password)
{
    settings_t c;
    settings_copy(&c);
    if (!c.web_hash[0] || strcmp(user, c.web_user) != 0) {
        return false;
    }
    char h[65];
    hash_pw(c.web_salt, password, h);
    // constant-time compare
    unsigned diff = 0;
    for (int i = 0; i < 64; i++) {
        diff |= (unsigned)(h[i] ^ c.web_hash[i]);
    }
    return diff == 0;
}

bool settings_web_configured(void)
{
    return s_cfg.web_hash[0] != 0;
}
