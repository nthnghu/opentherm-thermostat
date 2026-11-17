#include "ui_theme.h"
#include "esp_attr.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include "esp_log.h"
#include "ui.h"

static const char *TAG = "theme";

typedef struct {
    uint32_t dark, light;
} pair_t;

// Light values must be unique and must not be used anywhere else in the UI (they are matched exactly).
static const pair_t MAP[] = {
    {UI_BG, 0xF2F4F7},       {UI_SURFACE, 0xFEFEFE}, {UI_SURFACE_ALT, 0xE4E9EE}, {UI_STATUSBAR, 0xDAE0E7},
    {UI_TEXT, 0x1C2733},     {UI_MUTED, 0x546E7A},   {UI_FAINT, 0x7B8D98},       {UI_CARD_TEXT, 0x3E4F59},
    {UI_VALUE, 0x0277BD},    {UI_SELECTED, 0xD6E6F5}, {UI_DOT_OFF, 0xB8C2CA},    {UI_OK, 0x2E7D32},
    {UI_WARN, 0xEF6C00},     {UI_ERR, 0xC0392B},      {UI_HEAT, 0xE64A19},
};
#define MAP_N (sizeof(MAP) / sizeof(MAP[0]))

static bool s_dark = true;

uint32_t ui_hex(uint32_t dark_hex)
{
    if (s_dark) {
        return dark_hex;
    }
    for (size_t i = 0; i < MAP_N; i++) {
        if (MAP[i].dark == dark_hex) {
            return MAP[i].light;
        }
    }
    return dark_hex;
}

lv_color_t ui_c(uint32_t dark_hex)
{
    return lv_color_hex(ui_hex(dark_hex));
}

bool ui_theme_is_dark(void)
{
    return s_dark;
}

static const lv_style_prop_t PROPS[] = {LV_STYLE_BG_COLOR, LV_STYLE_TEXT_COLOR, LV_STYLE_BORDER_COLOR, LV_STYLE_ARC_COLOR,
                                        LV_STYLE_LINE_COLOR};
static const lv_style_selector_t SELS[] = {LV_PART_MAIN, LV_PART_INDICATOR, LV_PART_ITEMS, LV_PART_KNOB};

static lv_obj_tree_walk_res_t remap_cb(lv_obj_t *obj, void *user)
{
    bool to_dark = (bool)(intptr_t)user;
    for (size_t s = 0; s < sizeof(SELS) / sizeof(SELS[0]); s++) {
        for (size_t p = 0; p < sizeof(PROPS) / sizeof(PROPS[0]); p++) {
            lv_style_value_t v;
            if (lv_obj_get_local_style_prop(obj, PROPS[p], &v, SELS[s]) != LV_STYLE_RES_FOUND) {
                continue;
            }
            uint32_t hex = lv_color_to_u32(v.color) & 0xFFFFFF;
            for (size_t i = 0; i < MAP_N; i++) {
                uint32_t from = to_dark ? MAP[i].light : MAP[i].dark;
                if (hex == from) {
                    v.color = lv_color_hex(to_dark ? MAP[i].dark : MAP[i].light);
                    lv_obj_set_local_style_prop(obj, PROPS[p], v, SELS[s]);
                    break;
                }
            }
        }
    }
    return LV_OBJ_TREE_WALK_NEXT;
}

static void init_lvgl_theme(bool dark)
{
    lv_display_t *disp = lv_display_get_default();
    lv_theme_t *th = lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_DEEP_ORANGE),
                                           dark, &font_icons_20);  // Montserrat 20 plus the UI_ICON_* glyphs
    lv_display_set_theme(disp, th);
}

// The theme in use, kept over a software restart (OTA, watchdog): starting dark and switching to light a few
// seconds later, once the clock / HA's sun state is known, flashed the whole screen after every restart.
#define THEME_MAGIC 0x54480000u
static RTC_NOINIT_ATTR uint32_t s_saved_theme;

void ui_theme_init(void)
{
    s_dark = (s_saved_theme & 0xFFFF0000u) == THEME_MAGIC ? (s_saved_theme & 1) != 0 : true;
    init_lvgl_theme(s_dark);
}

void ui_theme_apply(bool dark)
{
    if (dark == s_dark) {
        return;
    }
    s_dark = dark;
    s_saved_theme = THEME_MAGIC | (dark ? 1 : 0);
    init_lvgl_theme(dark);
    lv_obj_tree_walk(NULL, remap_cb, (void *)(intptr_t)dark);           // every screen
    lv_obj_tree_walk(lv_layer_top(), remap_cb, (void *)(intptr_t)dark); // status bar, quick panel
    lv_obj_invalidate(lv_layer_top());
    ESP_LOGI(TAG, "%s theme", dark ? "dark" : "light");
}

static char s_buf[2048];
static char s_fmt[2048];

void ui_label_text(lv_obj_t *label, const char *text)
{
    if (s_dark || !strchr(text, '#')) {
        lv_label_set_text(label, text);
        return;
    }
    size_t o = 0;
    for (const char *p = text; *p && o < sizeof(s_buf) - 8; p++) {
        if (p[0] == '#' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2]) && isxdigit((unsigned char)p[3]) &&
            isxdigit((unsigned char)p[4]) && isxdigit((unsigned char)p[5]) && isxdigit((unsigned char)p[6])) {
            char hex[7];
            memcpy(hex, p + 1, 6);
            hex[6] = 0;
            o += snprintf(s_buf + o, sizeof(s_buf) - o, "#%06lX", (unsigned long)ui_hex(strtoul(hex, NULL, 16)));
            p += 6;
        } else {
            s_buf[o++] = *p;
        }
    }
    s_buf[o] = 0;
    lv_label_set_text(label, s_buf);
}

void ui_label_fmt(lv_obj_t *label, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_fmt, sizeof(s_fmt), fmt, ap);
    va_end(ap);
    ui_label_text(label, s_fmt);
}
