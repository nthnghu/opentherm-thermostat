#pragma once
#include <stdbool.h>
#include "lvgl.h"

#define UI_STATUS_H  36   // status bar, always on top (lv_layer_top)
#define UI_HEADER_H  64   // app header below it
#define UI_CONTENT_H (800 - UI_STATUS_H - UI_HEADER_H)

// Icons beyond LVGL's built-in LV_SYMBOL_* set (Font Awesome 5, tools/gen_font.py --icons). font_icons_20 is the
// UI's default font (it falls back to Montserrat 20), so these work in any label that uses the default font;
// font_icons_48 is for the launcher tiles.
#define UI_ICON_CLOCK   "\xEF\x80\x97"
#define UI_ICON_GLOBE   "\xEF\x82\xAC"
#define UI_ICON_THERMO  "\xEF\x8B\x89"
#define UI_ICON_CALC    "\xEF\x87\xAC"
#define UI_ICON_GAME    "\xEF\x84\x9B"
#define UI_ICON_PALETTE "\xEF\x94\xBF"
#define UI_ICON_SUN     "\xEF\x86\x85"
#define UI_ICON_WALK    "\xEF\x95\x94"
#define UI_ICON_DISPLAY "\xEF\x84\x88"
#define UI_ICON_DIAG    "\xEF\x83\xB1"
#define UI_ICON_FIRE    "\xEF\x81\xAD"
#define UI_ICON_DROP    "\xEF\x81\x83"
LV_FONT_DECLARE(font_icons_20);
LV_FONT_DECLARE(font_icons_48);

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
// Turns the backlight off until the next touch (or camera motion).
void ui_screen_off_now(void);
// Re-evaluates the day/night theme now (after the theme setting changed).
void ui_theme_update(void);
// Opens Setup with one section shown (index as in app_setup.c), or the Setup menu for -1.
void ui_open_setup_section(int section);

// Apps: each creates its screen once.
lv_obj_t *app_thermo_create(void);
lv_obj_t *app_ha_create(void);
lv_obj_t *app_calc_create(void);
lv_obj_t *app_blocks_create(void);
lv_obj_t *app_setup_create(void);

void app_thermo_show_page(int page);
int app_thermo_page(void);  // the page shown in the thermostat app (0 = main)
void app_setup_open_panel(int i);  // -1 = close all
// Debug (console GO ...): "launcher", "thermo", "ha", "calc", "blocks", "setup", "page N", "panel N".
void ui_debug_go(const char *what);
// Frames drawn, the time they took and the slowest one since the last call (console: PERF).
void ui_perf(uint32_t *frames, uint32_t *total_ms, uint32_t *max_ms);
uint32_t ui_perf_redraw(int n, bool blank);  // console: PERF redraw, PERF blank
