// Sensors app: lists every sensor.* entity from Home Assistant's REST API.
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_lvgl_port.h"
#include "cJSON.h"
#include "board.h"
#include "ui.h"
#include "settings.h"

static const char *TAG = "app_ha";

#define MAX_SENSORS   400
#define MAX_BODY      (2 * 1024 * 1024)
#define REFRESH_MS    30000

typedef struct {
    char name[64];
    char value[40];
} sensor_row_t;

static lv_obj_t *s_scr, *s_list, *s_status;
static TaskHandle_t s_task;
static volatile bool s_visible;

static sensor_row_t *s_rows;  // MAX_SENSORS, in PSRAM
static int s_row_count;

static void set_status(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lvgl_port_lock(0);
    lv_label_set_text(s_status, buf);
    lvgl_port_unlock();
}

// Returns a malloc'd, NUL-terminated body (caller frees) or NULL; *status gets the HTTP status.
static char *http_get_states(int *status)
{
    char url[256];
    const settings_t *st = settings_get();
    snprintf(url, sizeof(url), "%s/api/states", st->ha_url);
    char auth[400];
    snprintf(auth, sizeof(auth), "Bearer %s", st->ha_token);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 10000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Authorization", auth);
    esp_http_client_set_header(c, "Content-Type", "application/json");

    char *body = NULL;
    *status = -1;
    if (esp_http_client_open(c, 0) != ESP_OK) {
        goto done;
    }
    esp_http_client_fetch_headers(c);
    *status = esp_http_client_get_status_code(c);
    if (*status != 200) {
        goto done;
    }

    size_t cap = 64 * 1024, len = 0;
    body = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    while (body) {
        if (len + 4096 + 1 > cap) {
            if (cap >= MAX_BODY) {
                break;
            }
            cap *= 2;
            char *nb = heap_caps_realloc(body, cap, MALLOC_CAP_SPIRAM);
            if (!nb) {
                break;
            }
            body = nb;
        }
        int n = esp_http_client_read(c, body + len, 4096);
        if (n <= 0) {
            break;
        }
        len += n;
    }
    if (body) {
        body[len] = 0;
    }
done:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return body;
}

static int cmp_rows(const void *a, const void *b)
{
    return strcasecmp(((const sensor_row_t *)a)->name, ((const sensor_row_t *)b)->name);
}

static int parse_states(const char *body)
{
    cJSON *root = cJSON_Parse(body);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return -1;
    }
    int n = 0;
    cJSON *ent;
    cJSON_ArrayForEach(ent, root) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(ent, "entity_id"));
        if (!id || strncmp(id, "sensor.", 7) != 0 || n >= MAX_SENSORS) {
            continue;
        }
        const char *state = cJSON_GetStringValue(cJSON_GetObjectItem(ent, "state"));
        cJSON *attr = cJSON_GetObjectItem(ent, "attributes");
        const char *fname = cJSON_GetStringValue(cJSON_GetObjectItem(attr, "friendly_name"));
        const char *unit = cJSON_GetStringValue(cJSON_GetObjectItem(attr, "unit_of_measurement"));

        strlcpy(s_rows[n].name, fname ? fname : id + 7, sizeof(s_rows[n].name));
        snprintf(s_rows[n].value, sizeof(s_rows[n].value), "%s%s%s", state ? state : "?", unit ? " " : "",
                 unit ? unit : "");
        n++;
    }
    cJSON_Delete(root);
    qsort(s_rows, n, sizeof(sensor_row_t), cmp_rows);
    return n;
}

static void rebuild_list(void)
{
    lv_obj_clean(s_list);
    for (int i = 0; i < s_row_count; i++) {
        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_ver(row, 10, 0);
        lv_obj_set_style_pad_hor(row, 12, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, 260);
        lv_label_set_text(name, s_rows[i].name);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *val = lv_label_create(row);
        lv_obj_set_style_text_color(val, lv_color_hex(0x4FC3F7), 0);
        lv_label_set_long_mode(val, LV_LABEL_LONG_DOT);
        lv_obj_set_width(val, 160);
        lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(val, s_rows[i].value);
        lv_obj_align(val, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

static void fetch_once(void)
{
    if (!settings_get()->ha_token[0]) {
        set_status("HA not set up - open Setup on the home screen");
        return;
    }
    if (!board_wifi_connected()) {
        set_status("%s", board_wifi_status());
        return;
    }
    set_status("Loading...");
    int status;
    char *body = http_get_states(&status);
    if (!body) {
        if (status == 401) {
            set_status("HA rejected the token (401)");
        } else if (status < 0) {
            set_status("Can't reach %s", settings_get()->ha_url);
        } else {
            set_status("HA error: HTTP %d", status);
        }
        return;
    }
    int n = parse_states(body);
    free(body);
    if (n < 0) {
        set_status("Bad JSON from HA (response too large?)");
        return;
    }

    lvgl_port_lock(0);
    s_row_count = n;
    rebuild_list();
    lvgl_port_unlock();
    set_status("%d sensors  " LV_SYMBOL_REFRESH " every %ds", n, REFRESH_MS / 1000);
    ESP_LOGI(TAG, "loaded %d sensors", n);
}

static void fetch_task(void *arg)
{
    for (;;) {
        // Wait for a refresh request, or time out for the periodic refresh.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(REFRESH_MS));
        if (s_visible) {
            fetch_once();
        }
    }
}

static void refresh_cb(lv_event_t *e)
{
    xTaskNotifyGive(s_task);
}

static void screen_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SCREEN_LOADED) {
        s_visible = true;
        xTaskNotifyGive(s_task);
    } else if (code == LV_EVENT_SCREEN_UNLOADED) {
        s_visible = false;
    }
}

lv_obj_t *app_ha_create(void)
{
    s_rows = heap_caps_calloc(MAX_SENSORS, sizeof(sensor_row_t), MALLOC_CAP_SPIRAM);

    lv_obj_t *content;
    s_scr = ui_app_screen("Sensors", &content);
    lv_obj_add_event_cb(s_scr, screen_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(s_scr, screen_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

    s_status = lv_label_create(content);
    lv_obj_set_width(s_status, 330);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0x90A4AE), 0);
    lv_label_set_text(s_status, "");
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 16, 22);

    lv_obj_t *btn = ui_button(content, LV_SYMBOL_REFRESH " Refresh", lv_color_hex(0x1565C0), refresh_cb, NULL);
    lv_obj_set_size(btn, 120, 50);
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -10, 8);

    s_list = lv_obj_create(content);
    lv_obj_set_size(s_list, BOARD_LCD_H_RES, BOARD_LCD_V_RES - UI_HEADER_H - 70);
    lv_obj_align(s_list, LV_ALIGN_TOP_LEFT, 0, 70);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_list, 8, 0);
    lv_obj_set_style_pad_row(s_list, 6, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    xTaskCreate(fetch_task, "ha_fetch", 10240, NULL, 4, &s_task);
    return s_scr;
}
