#include "ha.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "settings.h"
#include <time.h>
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "esp_timer.h"

static const char *TAG = "ha";

#define MAX_BODY (2 * 1024 * 1024)

static char s_last[128];
static volatile ha_state_t s_state;

char *ha_get(const char *path, int *status)
{
    *status = -1;
    settings_t *c = malloc(sizeof(settings_t));
    if (!c) {
        return NULL;
    }
    settings_copy(c);
    if (!c->ha_url[0] || !c->ha_token[0]) {
        free(c);
        return NULL;
    }
    char url[256];
    snprintf(url, sizeof(url), "%s%s", c->ha_url, path);
    char auth[340];
    snprintf(auth, sizeof(auth), "Bearer %s", c->ha_token);
    free(c);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 8000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) {
        return NULL;
    }
    esp_http_client_set_header(h, "Authorization", auth);
    char *body = NULL;
    if (esp_http_client_open(h, 0) != ESP_OK) {
        s_state = HA_STATE_ERROR;
        goto done;
    }
    esp_http_client_fetch_headers(h);
    *status = esp_http_client_get_status_code(h);
    s_state = *status == 200 ? HA_STATE_OK : HA_STATE_ERROR;
    if (*status != 200) {
        ESP_LOGW(TAG, "GET %s -> HTTP %d (token %u chars)", path, *status, (unsigned)strlen(auth) - 7);
    }
    if (*status != 200) {
        goto done;
    }
    size_t cap = 16 * 1024, len = 0;
    body = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    while (body) {
        if (len + 4096 + 1 > cap) {
            if (cap >= MAX_BODY) {
                break;
            }
            char *nb = heap_caps_realloc(body, cap * 2, MALLOC_CAP_SPIRAM);
            if (!nb) {
                break;
            }
            body = nb;
            cap *= 2;
        }
        int n = esp_http_client_read(h, body + len, 4096);
        if (n <= 0) {
            break;
        }
        len += n;
    }
    if (body) {
        body[len] = 0;
    }
done:
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return body;
}

bool ha_test(char *msg, size_t size)
{
    const settings_t *c = settings_get();
    bool ok = false;
    if (!c->ha_url[0] || !c->ha_token[0]) {
        snprintf(msg, size, "HA URL or token missing");
    } else {
        int status;
        char *body = ha_get("/api/", &status);
        free(body);
        if (status == 200) {
            snprintf(msg, size, "Connected to Home Assistant");
            ok = true;
        } else if (status == 401) {
            snprintf(msg, size, "HA rejected the token (401)");
        } else if (status < 0) {
            snprintf(msg, size, "Can't reach %s", c->ha_url);
        } else {
            snprintf(msg, size, "HA answered HTTP %d", status);
        }
    }
    strlcpy(s_last, msg, sizeof(s_last));
    ESP_LOGI(TAG, "test: %s", msg);
    return ok;
}

const char *ha_last_result(void)
{
    return s_last;
}

ha_state_t ha_state(void)
{
    return s_state;
}

bool ha_token_plausible(const char *s)
{
    size_t n = strlen(s);
    if (n < 100 || n > 310) {
        return false;
    }
    int dots = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '.') {
            dots++;
        } else if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') {
            return false;
        }
    }
    return dots == 2;
}

void ha_token_describe(const char *token, char *out, size_t size)
{
    size_t n = strlen(token);
    const char *a = strchr(token, '.');
    const char *b = a ? strchr(a + 1, '.') : NULL;
    if (!a || !b) {
        snprintf(out, size, "%u chars, not a JWT", (unsigned)n);
        return;
    }
    // Payload = base64url between the dots -> standard base64 with padding.
    char b64[256];
    size_t len = b - a - 1, k = 0;
    for (size_t i = 0; i < len && k < sizeof(b64) - 4; i++) {
        char c = a[1 + i];
        b64[k++] = c == '-' ? '+' : c == '_' ? '/' : c;
    }
    while (k % 4) {
        b64[k++] = '=';
    }
    b64[k] = 0;
    unsigned char js[200];
    size_t jl = 0;
    long long iat = 0;
    if (mbedtls_base64_decode(js, sizeof(js) - 1, &jl, (unsigned char *)b64, k) == 0) {
        js[jl] = 0;
        cJSON *j = cJSON_Parse((char *)js);
        cJSON *v = cJSON_GetObjectItem(j, "iat");
        if (cJSON_IsNumber(v)) {
            iat = (long long)v->valuedouble;
        }
        cJSON_Delete(j);
    }
    if (iat) {
        time_t t = (time_t)iat;
        struct tm lt;
        localtime_r(&t, &lt);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", &lt);
        snprintf(out, size, "%u chars, issued %s, ends ...%s", (unsigned)n, ts, n > 4 ? token + n - 4 : "");
    } else {
        snprintf(out, size, "%u chars, payload unreadable", (unsigned)n);
    }
}

static volatile int64_t s_sun_us;  // when sun.sun was last read (0 = never)
static volatile bool s_sun_up;

void ha_sun_refresh(void)
{
    int status;
    char *body = ha_get("/api/states/sun.sun", &status);
    if (!body) {
        return;
    }
    cJSON *j = cJSON_Parse(body);
    free(body);
    const char *state = cJSON_GetStringValue(cJSON_GetObjectItem(j, "state"));
    if (state && (strcmp(state, "above_horizon") == 0 || strcmp(state, "below_horizon") == 0)) {
        bool up = strcmp(state, "above_horizon") == 0;
        if (up != s_sun_up || !s_sun_us) {
            ESP_LOGI(TAG, "sun.sun: %s", state);
        }
        s_sun_up = up;
        s_sun_us = esp_timer_get_time();
    }
    cJSON_Delete(j);
}

bool ha_sun_is_up(bool *up)
{
    if (!s_sun_us || esp_timer_get_time() - s_sun_us > 30LL * 60 * 1000000) {
        return false;
    }
    *up = s_sun_up;
    return true;
}
