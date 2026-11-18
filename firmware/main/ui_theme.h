#pragma once
// Day (light) / night (dark) theme.
// Screens are written with the dark palette colours below. On a switch, every object's local colours
// are remapped dark <-> light, and LVGL's default theme is re-initialised. New objects created while
// the light theme is active take ui_c() so they start out right.
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

// Neutral palette (dark values); each has a light counterpart in ui_theme.c.
#define UI_BG          0x121212  // screen / panel background
#define UI_SURFACE     0x1E2A38  // headers, cards
#define UI_SURFACE_ALT 0x0D141B  // boxes: calculator display, lists, diagnostics
#define UI_STATUSBAR   0x0B1117
#define UI_TEXT        0xE6EDF3
#define UI_MUTED       0x90A4AE
#define UI_FAINT       0x607D8B
#define UI_CARD_TEXT   0xB0BEC5
#define UI_VALUE       0x4FC3F7  // values, accent text
#define UI_SELECTED    0x263C52
#define UI_DOT_OFF     0x455A65
#define UI_OK          0x81C784
#define UI_WARN        0xFFB74D
#define UI_ERR         0xE57373
#define UI_HEAT        0xFF7043

void ui_theme_init(void);           // after the display exists; starts dark
void ui_theme_apply(bool dark);     // switches everything in place (LVGL lock held by caller)
bool ui_theme_is_dark(void);
lv_color_t ui_c(uint32_t dark_hex); // colour for the current theme
uint32_t ui_hex(uint32_t dark_hex); // same, for "#RRGGBB text#" recolour strings

// lv_label_set_text(_fmt) that also rewrites "#RRGGBB" recolour codes to the current theme.
// Call with the LVGL lock held (uses a shared buffer).
void ui_label_text(lv_obj_t *label, const char *text);
void ui_label_fmt(lv_obj_t *label, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// A scrolling list of tappable rows (replaces lv_list, deprecated in LVGL 9.6): a flex column box,
// and a full-width row with an optional icon and a wrapping text. Size and place the list as any object.
lv_obj_t *ui_list_create(lv_obj_t *parent);
lv_obj_t *ui_list_add(lv_obj_t *list, const char *icon, const char *text);
