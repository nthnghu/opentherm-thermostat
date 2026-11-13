#pragma once
#include "lvgl.h"

#define UI_HEADER_H 72

// Creates an app screen with a header (‹ Home button + title).
// *content receives the area below the header (480 x 728, no padding, no scrolling).
lv_obj_t *ui_app_screen(const char *title, lv_obj_t **content);

lv_obj_t *ui_button(lv_obj_t *parent, const char *text, lv_color_t color, lv_event_cb_t cb, void *user);

void ui_home_create(void);
void ui_go_home(void);
void ui_open_setup(void);

// Apps: each creates its screen once; the screen is loaded when its home button is tapped.
lv_obj_t *app_ha_create(void);
lv_obj_t *app_voice_create(void);
lv_obj_t *app_blocks_create(void);
lv_obj_t *app_setup_create(void);
