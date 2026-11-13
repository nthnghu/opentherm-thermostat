#pragma once
#include <stdbool.h>
#include "lvgl.h"

#define UI_STATUS_H  36   // status bar, always on top (lv_layer_top)
#define UI_HEADER_H  64   // app header below it
#define UI_CONTENT_H (800 - UI_STATUS_H - UI_HEADER_H)

// Creates an app screen: status bar space, header (‹ Apps button + title), content (480 x UI_CONTENT_H).
lv_obj_t *ui_app_screen(const char *title, lv_obj_t **content);
// Like ui_app_screen, but "‹ Back" returns to parent_screen.
lv_obj_t *ui_sub_screen(const char *title, lv_obj_t **content, lv_obj_t *parent_screen);
// A screen with only the status bar space reserved; *content is 480 x (800 - UI_STATUS_H).
lv_obj_t *ui_plain_screen(lv_obj_t **content);

lv_obj_t *ui_button(lv_obj_t *parent, const char *text, lv_color_t color, lv_event_cb_t cb, void *user);
// Full-size panel inside parent, hidden at first (for sub-pages and dialogs).
lv_obj_t *ui_overlay(lv_obj_t *parent);

void ui_shell_create(void);   // status bar, launcher, apps; shows the thermostat
void ui_go_launcher(void);
void ui_go_thermostat(void);
void ui_open_setup(void);
// While true, the inactivity timer does not switch back to the thermostat (camera scan, keyboard).
void ui_set_busy(bool busy);

// Apps: each creates its screen once.
lv_obj_t *app_thermo_create(void);
lv_obj_t *app_ha_create(void);
lv_obj_t *app_calc_create(void);
lv_obj_t *app_blocks_create(void);
lv_obj_t *app_setup_create(void);

void app_thermo_show_page(int page);
void app_setup_open_panel(int i);  // -1 = close all
// Debug (USB serial): "launcher", "thermo", "ha", "calc", "blocks", "setup", "page N", "panel N".
void ui_debug_go(const char *what);
