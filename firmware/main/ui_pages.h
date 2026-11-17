#pragma once
// Thermostat app pages 3-5 (milestone 6): boiler card, schedule, settings (advanced editor, service with PIN).
#include "lvgl.h"

void ui_pages_init(lv_obj_t *thermo_screen);  // the screen to come back to from the editors

void ui_pages_boiler(lv_obj_t *tile);
void ui_pages_boiler_update(void);
void ui_pages_schedule(lv_obj_t *tile);
void ui_pages_schedule_update(void);
void ui_pages_settings(lv_obj_t *tile);
void ui_pages_settings_update(void);

// Opens the list of one settings group (cfg_group_t); its Back button returns to the Settings list (app_setup.c).
void ui_pages_open_group(int g);

// Debug (console GO): "edit <name>", "advanced", "service", "day N", "group N"
void ui_pages_debug(const char *what);
