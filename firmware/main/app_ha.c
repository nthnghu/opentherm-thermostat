// Home Assistant app.
// Dashboard: swipeable pages, each a grid of entity cards (name + value). Values are polled every 15 s
// for the page on screen. Edit screen ("Edit"): pages (add, rename, delete) and cards (add from HA's
// entity list, remove, move up/down, move to another page). The layout is stored in NVS (hadash.c).
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include <stdlib.h>
#include <strings.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "cJSON.h"
#include "board.h"
#include "ui.h"
#include "ui_theme.h"
#include "settings.h"
#include "ha.h"
#include "hadash.h"

static const char *TAG = "app_ha";

#define POLL_MS      15000
#define MAX_CHOICES  3000
#define MAX_SHOWN    60
#define CACHE_LEN    (HADASH_MAX_PAGES * HADASH_MAX_PER_PAGE)
#define CARD_W       222
#define CARD_H       128
#define DASH_TOP     64

typedef struct {
    char id[HADASH_ID_LEN];
    char name[64];
    char value[40];
    bool ok;
} entity_val_t;

typedef struct {
    char id[64];
    char name[64];
} choice_t;

typedef enum { REQ_POLL = 1, REQ_LIST = 2 } req_t;

// Shared between the LVGL task and the worker: guarded by s_lock.
static SemaphoreHandle_t s_lock;
static hadash_t s_dash;
static entity_val_t s_cache[CACHE_LEN];
static int s_cur_page;  // page on the dashboard
static volatile bool s_visible;

// LVGL-only state
static lv_obj_t *s_scr, *s_tv, *s_title, *s_status, *s_dots_box;
static lv_obj_t *s_card_name[HADASH_MAX_PAGES][HADASH_MAX_PER_PAGE], *s_card_val[HADASH_MAX_PAGES][HADASH_MAX_PER_PAGE];
static bool s_dash_dirty = true;

static lv_obj_t *s_ed_scr, *s_ed_page_lbl, *s_ed_list, *s_ed_msg, *s_tool[5];
static int s_ed_page, s_ed_sel = -1;
static lv_obj_t *s_rename, *s_rename_ta;

static lv_obj_t *s_picker, *s_search, *s_plist, *s_pstatus;
static choice_t *s_choices;  // PSRAM
static int s_choice_count;

static TaskHandle_t s_task;

/* ---------------------------------------------------------------- cache */

// Caller holds s_lock.
static entity_val_t *cache_get(const char *id, bool create)
{
    for (int i = 0; i < CACHE_LEN; i++) {
        if (s_cache[i].id[0] && strcmp(s_cache[i].id, id) == 0) {
            return &s_cache[i];
        }
    }
    if (!create) {
        return NULL;
    }
    for (int i = 0; i < CACHE_LEN; i++) {
        if (!s_cache[i].id[0]) {
            memset(&s_cache[i], 0, sizeof(s_cache[i]));
            strlcpy(s_cache[i].id, id, sizeof(s_cache[i].id));
            return &s_cache[i];
        }
    }
    return NULL;
}

static void display_name(const char *id, char *out, size_t n)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    entity_val_t *v = cache_get(id, false);
    strlcpy(out, v && v->name[0] ? v->name : id, n);
    xSemaphoreGive(s_lock);
}

static void dash_changed(void)
{
    hadash_save(&s_dash);
    s_dash_dirty = true;
}

/* ------------------------------------------------------------ dashboard */

static void set_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
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

static void update_title(void)
{
    lv_label_set_text(s_title, s_dash.pages[s_cur_page].name);
    for (uint32_t i = 0; i < lv_obj_get_child_count(s_dots_box); i++) {
        lv_obj_set_style_bg_color(lv_obj_get_child(s_dots_box, i),
                                  ui_c(i == (uint32_t)s_cur_page ? UI_TEXT : UI_DOT_OFF), 0);
    }
}

// Writes the cached values into the cards of one page.
static void refresh_values(int page)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int k = 0; k < s_dash.pages[page].count; k++) {
        if (!s_card_name[page][k]) {
            continue;
        }
        entity_val_t *v = cache_get(s_dash.pages[page].ids[k], false);
        lv_label_set_text(s_card_name[page][k], v && v->name[0] ? v->name : s_dash.pages[page].ids[k]);
        lv_label_set_text(s_card_val[page][k], v && v->value[0] ? v->value : "...");
        lv_obj_set_style_text_color(s_card_val[page][k], ui_c(v && v->ok ? 0x4FC3F7 : 0x90A4AE), 0);
    }
    xSemaphoreGive(s_lock);
}

static void tile_cb(lv_event_t *e)
{
    lv_obj_t *act = lv_tileview_get_tile_active(s_tv);
    s_cur_page = act ? (int)lv_obj_get_index(act) : 0;
    update_title();
    xTaskNotify(s_task, REQ_POLL, eSetBits);
}

static void build_dashboard(void)
{
    lv_obj_clean(s_tv);
    lv_obj_clean(s_dots_box);
    memset(s_card_name, 0, sizeof(s_card_name));
    memset(s_card_val, 0, sizeof(s_card_val));
    if (s_cur_page >= s_dash.page_count) {
        s_cur_page = 0;
    }
    for (int p = 0; p < s_dash.page_count; p++) {
        lv_dir_t dir = (p > 0 ? LV_DIR_LEFT : 0) | (p < s_dash.page_count - 1 ? LV_DIR_RIGHT : 0);
        lv_obj_t *tile = lv_tileview_add_tile(s_tv, p, 0, dir);
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_all(tile, 12, 0);
        lv_obj_set_style_pad_row(tile, 12, 0);
        lv_obj_set_style_pad_column(tile, 12, 0);
        hadash_page_t *pg = &s_dash.pages[p];
        if (pg->count == 0) {
            lv_obj_t *l = lv_label_create(tile);
            lv_obj_set_width(l, 440);
            lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_color(l, ui_c(0x90A4AE), 0);
            lv_label_set_text(l, "No cards on this page yet. Tap \"Edit\" to add sensors from Home Assistant.");
        }
        for (int k = 0; k < pg->count; k++) {
            lv_obj_t *card = lv_obj_create(tile);
            lv_obj_set_size(card, CARD_W, CARD_H);
            lv_obj_set_style_radius(card, 12, 0);
            lv_obj_set_style_pad_all(card, 12, 0);
            lv_obj_set_style_bg_color(card, ui_c(0x1E2A38), 0);
            lv_obj_set_style_border_width(card, 0, 0);
            lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

            lv_obj_t *name = lv_label_create(card);
            lv_obj_set_size(name, CARD_W - 24, 48);  // fixed height: at most 2 lines, then "..."
            lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_color(name, ui_c(0xB0BEC5), 0);
            lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);
            s_card_name[p][k] = name;

            lv_obj_t *val = lv_label_create(card);
            lv_obj_set_size(val, CARD_W - 24, 34);
            lv_label_set_long_mode(val, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_font(val, &lv_font_montserrat_28, 0);
            lv_obj_align(val, LV_ALIGN_BOTTOM_LEFT, 0, 0);
            s_card_val[p][k] = val;
        }
        refresh_values(p);

        lv_obj_t *dot = lv_obj_create(s_dots_box);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 10, 10);
        lv_obj_set_style_radius(dot, 5, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    }
    lv_obj_update_layout(s_tv);
    lv_tileview_set_tile_by_index(s_tv, s_cur_page, 0, LV_ANIM_OFF);
    if (s_dash.page_count < 2) {
        lv_obj_add_flag(s_dots_box, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_dots_box, LV_OBJ_FLAG_HIDDEN);
    }
    update_title();
    s_dash_dirty = false;
}

/* --------------------------------------------------------------- worker */

static void fill_value(entity_val_t *c, cJSON *ent)
{
    const char *state = cJSON_GetStringValue(cJSON_GetObjectItem(ent, "state"));
    cJSON *attr = cJSON_GetObjectItem(ent, "attributes");
    const char *fname = cJSON_GetStringValue(cJSON_GetObjectItem(attr, "friendly_name"));
    const char *unit = cJSON_GetStringValue(cJSON_GetObjectItem(attr, "unit_of_measurement"));
    if (fname) {
        strlcpy(c->name, fname, sizeof(c->name));
    }
    snprintf(c->value, sizeof(c->value), "%s%s%s", state ? state : "?", unit ? " " : "", unit ? unit : "");
    c->ok = state && strcmp(state, "unavailable") != 0 && strcmp(state, "unknown") != 0;
}

static void poll_page(void)
{
    if (!settings_get()->ha_token[0]) {
        set_status("HA not set up: Setup > Home Assistant");
        return;
    }
    if (!board_wifi_connected()) {
        set_status("%s", board_wifi_status());
        return;
    }
    static char ids[HADASH_MAX_PER_PAGE][HADASH_ID_LEN];  // worker task only
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int page = s_cur_page < s_dash.page_count ? s_cur_page : 0;
    int n = s_dash.pages[page].count;
    memcpy(ids, s_dash.pages[page].ids, sizeof(ids));
    xSemaphoreGive(s_lock);

    int errors = 0, last_status = 0;
    for (int i = 0; i < n; i++) {
        char path[96];
        snprintf(path, sizeof(path), "/api/states/%s", ids[i]);
        int status;
        char *body = ha_get(path, &status);
        entity_val_t tmp = {0};
        bool got = false;
        if (body) {
            cJSON *ent = cJSON_Parse(body);
            free(body);
            if (ent) {
                fill_value(&tmp, ent);
                cJSON_Delete(ent);
                got = true;
            }
        }
        if (!got) {
            errors++;
            last_status = status;
            strlcpy(tmp.value, status == 404 ? "not found" : "error", sizeof(tmp.value));
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        entity_val_t *c = cache_get(ids[i], true);
        if (c) {
            if (tmp.name[0]) {
                strlcpy(c->name, tmp.name, sizeof(c->name));
            }
            strlcpy(c->value, tmp.value, sizeof(c->value));
            c->ok = tmp.ok;
        }
        xSemaphoreGive(s_lock);
    }
    lvgl_port_lock(0);
    if (!s_dash_dirty && page < s_dash.page_count) {
        refresh_values(page);
    }
    lvgl_port_unlock();
    if (errors && last_status == 401) {
        set_status("HA rejected the token (401)");
    } else if (errors && last_status < 0) {
        set_status("Can't reach Home Assistant");
    } else {
        set_status("%d cards, every %d s", n, POLL_MS / 1000);
    }
}

static int cmp_choice(const void *a, const void *b)
{
    return strcasecmp(((const choice_t *)a)->id, ((const choice_t *)b)->id);
}

static void load_choices(void)
{
    lvgl_port_lock(0);
    lv_label_set_text(s_pstatus, "Loading entities from Home Assistant...");
    lvgl_port_unlock();
    int status;
    char *body = ha_get("/api/states", &status);
    cJSON *root = body ? cJSON_Parse(body) : NULL;
    free(body);
    int n = 0;
    cJSON *ent;
    cJSON_ArrayForEach(ent, root) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(ent, "entity_id"));
        if (!id || n >= MAX_CHOICES || strlen(id) >= HADASH_ID_LEN) {
            continue;
        }
        const char *fname =
            cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(ent, "attributes"), "friendly_name"));
        strlcpy(s_choices[n].id, id, sizeof(s_choices[n].id));
        strlcpy(s_choices[n].name, fname ? fname : "", sizeof(s_choices[n].name));
        n++;
    }
    cJSON_Delete(root);
    qsort(s_choices, n, sizeof(choice_t), cmp_choice);
    lvgl_port_lock(0);
    s_choice_count = n;
    if (n == 0) {
        lv_label_set_text_fmt(s_pstatus, "%s (HTTP %d)",
                              status == 401 ? "HA rejected the token" : "Can't load entities", status);
    } else {
        lv_obj_send_event(s_search, LV_EVENT_VALUE_CHANGED, NULL);
    }
    lvgl_port_unlock();
}

static void worker(void *arg)
{
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(ha_state() == HA_STATE_UNKNOWN ? 3000 : POLL_MS));
        if (bits & REQ_LIST) {
            load_choices();
        }
        // Check HA once Wi-Fi is up, so the status bar shows the connection state from boot on.
        if (ha_state() == HA_STATE_UNKNOWN && board_wifi_connected() && settings_get()->ha_token[0]) {
            char msg[128];
            ha_test(msg, sizeof(msg));
        }
        if (s_visible && ((bits & REQ_POLL) || bits == 0)) {
            poll_page();
        }
        // HA's sun state drives the automatic day/night theme (every 5 min, also when this app is closed).
        static int64_t last_sun;
        int64_t now = esp_timer_get_time();
        if (settings_get()->theme_mode == 0 && ha_state() == HA_STATE_OK && board_wifi_connected() &&
            (last_sun == 0 || now - last_sun > 5LL * 60 * 1000000)) {
            last_sun = now;
            ha_sun_refresh();
        }
    }
}

/* ---------------------------------------------------------------- editor */

static void editor_refresh(void);

static void row_cb(lv_event_t *e)
{
    s_ed_sel = (int)(intptr_t)lv_event_get_user_data(e);
    editor_refresh();
}

static void editor_refresh(void)
{
    if (s_ed_page >= s_dash.page_count) {
        s_ed_page = s_dash.page_count - 1;
    }
    hadash_page_t *pg = &s_dash.pages[s_ed_page];
    if (s_ed_sel >= pg->count) {
        s_ed_sel = pg->count - 1;
    }
    lv_label_set_text_fmt(s_ed_page_lbl, "Page %d of %d\n%s", s_ed_page + 1, s_dash.page_count, pg->name);
    lv_obj_clean(s_ed_list);
    for (int k = 0; k < pg->count; k++) {
        lv_obj_t *row = lv_obj_create(s_ed_list);
        lv_obj_set_size(row, LV_PCT(100), 62);
        lv_obj_set_style_pad_hor(row, 12, 0);
        lv_obj_set_style_pad_ver(row, 4, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_border_width(row, k == s_ed_sel ? 3 : 0, 0);
        lv_obj_set_style_border_color(row, ui_c(0x4FC3F7), 0);
        lv_obj_set_style_bg_color(row, ui_c(k == s_ed_sel ? 0x263C52 : 0x1E2A38), 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)k);
        char name[64];
        display_name(pg->ids[k], name, sizeof(name));
        lv_obj_t *n = lv_label_create(row);
        lv_obj_set_size(n, 400, 24);
        lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
        lv_label_set_text_fmt(n, "%d.  %s", k + 1, name);
        lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_t *id = lv_label_create(row);
        lv_obj_set_size(id, 400, 22);
        lv_label_set_long_mode(id, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(id, ui_c(0x607D8B), 0);
        lv_label_set_text(id, pg->ids[k]);
        lv_obj_align(id, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
    if (pg->count == 0) {
        lv_obj_t *l = lv_label_create(s_ed_list);
        lv_obj_set_style_text_color(l, ui_c(0x90A4AE), 0);
        lv_label_set_text(l, "No cards. Tap \"+ Add card\".");
    }
    bool sel = s_ed_sel >= 0;
    bool can[5] = {sel && s_ed_sel > 0, sel && s_ed_sel < pg->count - 1, sel && s_ed_page > 0,
                   sel && s_ed_page < s_dash.page_count - 1, sel};
    for (int i = 0; i < 5; i++) {
        if (can[i]) {
            lv_obj_remove_state(s_tool[i], LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_tool[i], LV_STATE_DISABLED);
        }
    }
}

static void ed_msg(const char *text)
{
    lv_label_set_text(s_ed_msg, text);
}

static void page_prev_cb(lv_event_t *e)
{
    if (s_ed_page > 0) {
        s_ed_page--;
        s_ed_sel = -1;
    }
    editor_refresh();
}

static void page_next_cb(lv_event_t *e)
{
    if (s_ed_page < s_dash.page_count - 1) {
        s_ed_page++;
        s_ed_sel = -1;
    }
    editor_refresh();
}

static void tool_cb(lv_event_t *e)
{
    int t = (int)(intptr_t)lv_event_get_user_data(e);
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    switch (t) {
    case 0:
        ok = hadash_move(&s_dash, s_ed_page, s_ed_sel, -1);
        if (ok) {
            s_ed_sel--;
        }
        break;
    case 1:
        ok = hadash_move(&s_dash, s_ed_page, s_ed_sel, 1);
        if (ok) {
            s_ed_sel++;
        }
        break;
    case 2:
    case 3: {
        int to = s_ed_page + (t == 2 ? -1 : 1);
        ok = hadash_move_to_page(&s_dash, s_ed_page, s_ed_sel, to);
        if (ok) {  // follow the card to its new page
            s_ed_page = to;
            s_ed_sel = s_dash.pages[to].count - 1;
        }
        break;
    }
    case 4:
        ok = hadash_remove(&s_dash, s_ed_page, s_ed_sel);
        break;
    }
    xSemaphoreGive(s_lock);
    if (ok) {
        dash_changed();
        ed_msg("");
    } else if (t == 2 || t == 3) {
        ed_msg("That page is full (12 cards).");
    }
    editor_refresh();
}

static void add_page_cb(lv_event_t *e)
{
    char name[24];
    snprintf(name, sizeof(name), "Page %d", s_dash.page_count + 1);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = hadash_add_page(&s_dash, name);
    xSemaphoreGive(s_lock);
    if (ok) {
        s_ed_page = s_dash.page_count - 1;
        s_ed_sel = -1;
        dash_changed();
        ed_msg("Page added. Tap Rename to name it.");
    } else {
        ed_msg("At most 6 pages.");
    }
    editor_refresh();
}

static void del_page_cb(lv_event_t *e)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = hadash_delete_page(&s_dash, s_ed_page);
    xSemaphoreGive(s_lock);
    if (ok) {
        if (s_ed_page > 0) {
            s_ed_page--;
        }
        s_ed_sel = -1;
        dash_changed();
        ed_msg("Page deleted; its cards moved to a neighbouring page.");
    } else {
        ed_msg("The last page can't be deleted.");
    }
    editor_refresh();
}

static void rename_open_cb(lv_event_t *e)
{
    lv_textarea_set_text(s_rename_ta, s_dash.pages[s_ed_page].name);
    lv_obj_remove_flag(s_rename, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_rename);
    ui_set_busy(true);
}

static void rename_kb_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY && lv_textarea_get_text(s_rename_ta)[0]) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(s_dash.pages[s_ed_page].name, lv_textarea_get_text(s_rename_ta), HADASH_NAME_LEN);
        xSemaphoreGive(s_lock);
        dash_changed();
    }
    lv_obj_add_flag(s_rename, LV_OBJ_FLAG_HIDDEN);
    ui_set_busy(false);
    editor_refresh();
}

/* ---------------------------------------------------------------- picker */

static void choice_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool dup = hadash_contains(&s_dash, s_choices[i].id);
    bool ok = !dup && hadash_add(&s_dash, s_ed_page, s_choices[i].id);
    if (ok && s_choices[i].name[0]) {
        entity_val_t *c = cache_get(s_choices[i].id, true);
        if (c) {
            strlcpy(c->name, s_choices[i].name, sizeof(c->name));
        }
    }
    int n = s_dash.pages[s_ed_page].count;
    xSemaphoreGive(s_lock);
    if (ok) {
        dash_changed();
        lv_label_set_text_fmt(s_pstatus, "Added to \"%s\" (%d/%d)", s_dash.pages[s_ed_page].name, n,
                              HADASH_MAX_PER_PAGE);
    } else {
        lv_label_set_text(s_pstatus, dup ? "Already on the dashboard" : "This page is full (12 cards)");
    }
}

static void search_cb(lv_event_t *e)
{
    const char *q = lv_textarea_get_text(s_search);
    lv_obj_clean(s_plist);
    int shown = 0, matches = 0;
    for (int i = 0; i < s_choice_count; i++) {
        if (q[0] && !strcasestr(s_choices[i].id, q) && !strcasestr(s_choices[i].name, q)) {
            continue;
        }
        matches++;
        if (shown >= MAX_SHOWN) {
            continue;
        }
        char text[140];
        snprintf(text, sizeof(text), "%s\n%s", s_choices[i].name[0] ? s_choices[i].name : s_choices[i].id,
                 s_choices[i].id);
        lv_obj_t *b = lv_list_add_button(s_plist, LV_SYMBOL_PLUS, text);
        lv_obj_add_event_cb(b, choice_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        shown++;
    }
    if (s_choice_count) {
        lv_label_set_text_fmt(s_pstatus, "%d matches%s. Tap to add.", matches, matches > shown ? " (first 60 shown)" : "");
    }
}

static void picker_open_cb(lv_event_t *e)
{
    lv_obj_remove_flag(s_picker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_picker);
    lv_textarea_set_text(s_search, "");
    ui_set_busy(true);
    xTaskNotify(s_task, REQ_LIST, eSetBits);
}

static void picker_done_cb(lv_event_t *e)
{
    lv_obj_add_flag(s_picker, LV_OBJ_FLAG_HIDDEN);
    ui_set_busy(false);
    editor_refresh();
}

/* --------------------------------------------------------------- screens */

static void edit_cb(lv_event_t *e)
{
    s_ed_page = s_cur_page;
    s_ed_sel = -1;
    ed_msg("Tap a card to select it, then use the buttons below.");
    editor_refresh();
    lv_screen_load_anim(s_ed_scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 180, 0, false);
}

static void refresh_cb(lv_event_t *e)
{
    xTaskNotify(s_task, REQ_POLL, eSetBits);
}

static void screen_cb(lv_event_t *e)
{
    s_visible = lv_event_get_code(e) == LV_EVENT_SCREEN_LOADED;
    if (s_visible) {
        if (s_dash_dirty) {
            build_dashboard();
        }
        xTaskNotify(s_task, REQ_POLL, eSetBits);
    }
}

static void ed_screen_cb(lv_event_t *e)
{
    lv_obj_add_flag(s_picker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_rename, LV_OBJ_FLAG_HIDDEN);
    ui_set_busy(false);
}

static lv_obj_t *btn(lv_obj_t *p, const char *t, uint32_t c, lv_event_cb_t cb, void *u, int x, int y, int w, int h)
{
    lv_obj_t *b = ui_button(p, t, ui_c(c), cb, u);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    return b;
}

static void build_editor(void)
{
    lv_obj_t *c;
    s_ed_scr = ui_sub_screen("Edit dashboard", &c, s_scr);
    lv_obj_add_event_cb(s_ed_scr, ed_screen_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

    btn(c, LV_SYMBOL_LEFT, 0x37474F, page_prev_cb, NULL, 12, 8, 64, 56);
    s_ed_page_lbl = lv_label_create(c);
    lv_obj_set_width(s_ed_page_lbl, 300);
    lv_obj_set_style_text_align(s_ed_page_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_ed_page_lbl, 90, 10);
    btn(c, LV_SYMBOL_RIGHT, 0x37474F, page_next_cb, NULL, 404, 8, 64, 56);

    btn(c, LV_SYMBOL_PLUS " Page", 0x2E7D32, add_page_cb, NULL, 12, 74, 144, 48);
    btn(c, LV_SYMBOL_EDIT " Rename", 0x455A64, rename_open_cb, NULL, 168, 74, 144, 48);
    btn(c, LV_SYMBOL_TRASH " Page", 0x8E2424, del_page_cb, NULL, 324, 74, 144, 48);

    s_ed_list = lv_obj_create(c);
    lv_obj_set_size(s_ed_list, 456, 386);
    lv_obj_set_pos(s_ed_list, 12, 132);
    lv_obj_set_style_pad_all(s_ed_list, 6, 0);
    lv_obj_set_style_pad_row(s_ed_list, 6, 0);
    lv_obj_set_style_bg_color(s_ed_list, ui_c(0x0D141B), 0);
    lv_obj_set_flex_flow(s_ed_list, LV_FLEX_FLOW_COLUMN);

    s_ed_msg = lv_label_create(c);
    lv_obj_set_width(s_ed_msg, 456);
    lv_label_set_long_mode(s_ed_msg, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_ed_msg, ui_c(0x90A4AE), 0);
    lv_obj_set_pos(s_ed_msg, 14, 524);

    static const char *tools[5] = {LV_SYMBOL_UP, LV_SYMBOL_DOWN, LV_SYMBOL_LEFT " Pg", "Pg " LV_SYMBOL_RIGHT, LV_SYMBOL_TRASH};
    static const uint32_t colors[5] = {0x1565C0, 0x1565C0, 0x455A64, 0x455A64, 0x8E2424};
    for (int i = 0; i < 5; i++) {
        s_tool[i] = btn(c, tools[i], colors[i], tool_cb, (void *)(intptr_t)i, 12 + i * 92, 556, 84, 60);
    }
    btn(c, LV_SYMBOL_PLUS " Add card", 0x2E7D32, picker_open_cb, NULL, 12, 628, 456, 60);

    // Picker: search box + matches + keyboard
    s_picker = ui_overlay(c);
    s_search = lv_textarea_create(s_picker);
    lv_textarea_set_one_line(s_search, true);
    lv_textarea_set_placeholder_text(s_search, "Search, e.g. temperature");
    lv_obj_set_size(s_search, 330, 52);
    lv_obj_set_pos(s_search, 12, 8);
    lv_obj_add_event_cb(s_search, search_cb, LV_EVENT_VALUE_CHANGED, NULL);
    btn(s_picker, "Done", 0x37474F, picker_done_cb, NULL, 352, 8, 116, 52);
    s_pstatus = lv_label_create(s_picker);
    lv_obj_set_style_text_color(s_pstatus, ui_c(0x90A4AE), 0);
    lv_obj_set_pos(s_pstatus, 14, 68);
    s_plist = lv_list_create(s_picker);
    lv_obj_set_size(s_plist, 456, UI_CONTENT_H - 96 - 260);
    lv_obj_set_pos(s_plist, 12, 96);
    lv_obj_t *kb = lv_keyboard_create(s_picker);
    lv_obj_set_size(kb, LV_PCT(100), 256);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, s_search);

    // Rename overlay
    s_rename = ui_overlay(c);
    lv_obj_t *rt = lv_label_create(s_rename);
    lv_label_set_text(rt, "Page name (check mark saves)");
    lv_obj_set_pos(rt, 16, 16);
    s_rename_ta = lv_textarea_create(s_rename);
    lv_textarea_set_one_line(s_rename_ta, true);
    lv_textarea_set_max_length(s_rename_ta, HADASH_NAME_LEN - 1);
    lv_obj_set_width(s_rename_ta, 448);
    lv_obj_set_pos(s_rename_ta, 16, 48);
    lv_obj_t *rkb = lv_keyboard_create(s_rename);
    lv_obj_set_size(rkb, LV_PCT(100), 320);
    lv_obj_align(rkb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(rkb, s_rename_ta);
    lv_obj_add_event_cb(rkb, rename_kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(rkb, rename_kb_cb, LV_EVENT_CANCEL, NULL);
}

lv_obj_t *app_ha_create(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_choices = heap_caps_calloc(MAX_CHOICES, sizeof(choice_t), MALLOC_CAP_SPIRAM);
    hadash_load(&s_dash);

    lv_obj_t *content;
    s_scr = ui_app_screen("Home Assistant", &content);
    lv_obj_add_event_cb(s_scr, screen_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(s_scr, screen_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

    s_title = lv_label_create(content);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_28, 0);
    lv_obj_set_size(s_title, 240, 34);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_title, 16, 4);
    s_status = lv_label_create(content);
    lv_obj_set_size(s_status, 250, 22);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_status, ui_c(0x90A4AE), 0);
    lv_label_set_text(s_status, "");
    lv_obj_set_pos(s_status, 16, 38);
    btn(content, LV_SYMBOL_REFRESH, 0x1565C0, refresh_cb, NULL, 270, 6, 70, 52);
    btn(content, LV_SYMBOL_EDIT " Edit", 0x455A64, edit_cb, NULL, 350, 6, 118, 52);

    s_tv = lv_tileview_create(content);
    lv_obj_set_size(s_tv, BOARD_LCD_H_RES, UI_CONTENT_H - DASH_TOP - 24);
    lv_obj_set_pos(s_tv, 0, DASH_TOP);
    lv_obj_set_style_bg_opa(s_tv, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(s_tv, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_tv, tile_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_dots_box = lv_obj_create(content);
    lv_obj_remove_style_all(s_dots_box);
    lv_obj_set_size(s_dots_box, 200, 16);
    lv_obj_align(s_dots_box, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_flex_flow(s_dots_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_dots_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_dots_box, 12, 0);

    build_editor();
    build_dashboard();

    // Stack in PSRAM: HTTP only, never flash, and internal DMA RAM is kept for the Wi-Fi link.
    xTaskCreatePinnedToCoreWithCaps(worker, "ha_app", 10240, NULL, 3, &s_task, 0, MALLOC_CAP_SPIRAM);
    int total = 0;
    for (int i = 0; i < s_dash.page_count; i++) {
        total += s_dash.pages[i].count;
    }
    ESP_LOGI(TAG, "%d pages, %d cards", s_dash.page_count, total);
    return s_scr;
}
